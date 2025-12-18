package resource

import (
	"fmt"
	"strconv"
	"sync"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/reflect/protoregistry"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/dynamicpb"
)

// Table 是一个运行期资源表：加载 pb 二进制后，基于主键与索引构建内存访问结构。
// 这是最小可用版本：支持单主键字段与单字段索引（可重复/唯一）。
//
// 约定：tableMessage 必须是一个 message，包含 repeated rows 字段。
// rows 的元素类型是资源“行” message。
type Table struct {
	mu sync.RWMutex

	TableDesc protoreflect.MessageDescriptor
	RowDesc   protoreflect.MessageDescriptor

	Rows []*dynamicpb.Message

	pkField protoreflect.FieldDescriptor
	byPK    map[string]*dynamicpb.Message

	// indexName -> key -> rows
	byIndex map[string]map[string][]*dynamicpb.Message

	primaryKeyExt protoreflect.ExtensionType
	indexExt      protoreflect.ExtensionType
}

// NewTableFromDescriptorSet 用 descriptor_set + message 全名（如 "game.res.ItemTable"）初始化 Table。
func NewTableFromDescriptorSet(fds *descriptorpb.FileDescriptorSet, tableMessageFullName string) (*Table, error) {
	files, err := protodesc.NewFiles(fds)
	if err != nil {
		return nil, fmt.Errorf("build protodesc files: %w", err)
	}

	md, err := findMessage(files, protoreflect.FullName(tableMessageFullName))
	if err != nil {
		return nil, err
	}

	rowsField := md.Fields().ByName("rows")
	if rowsField == nil || !rowsField.IsList() || rowsField.Kind() != protoreflect.MessageKind {
		return nil, fmt.Errorf("%s must have repeated message field named 'rows'", tableMessageFullName)
	}
	rowDesc := rowsField.Message()

	pkExtDesc := findExtension(files, "resource.options.primary_key")
	idxExtDesc := findExtension(files, "resource.options.index")
	var primaryKeyExt protoreflect.ExtensionType
	var indexExt protoreflect.ExtensionType
	if pkExtDesc != nil {
		primaryKeyExt = dynamicpb.NewExtensionType(pkExtDesc)
	}
	if idxExtDesc != nil {
		indexExt = dynamicpb.NewExtensionType(idxExtDesc)
	}

	pkField, err := findPrimaryKeyField(rowDesc, primaryKeyExt)
	if err != nil {
		return nil, err
	}

	t := &Table{
		TableDesc:     md,
		RowDesc:       rowDesc,
		Rows:          nil,
		pkField:       pkField,
		byPK:          map[string]*dynamicpb.Message{},
		byIndex:       map[string]map[string][]*dynamicpb.Message{},
		primaryKeyExt: primaryKeyExt,
		indexExt:      indexExt,
	}

	// 初始化索引容器（仅单字段索引）
	for i := 0; i < rowDesc.Fields().Len(); i++ {
		fd := rowDesc.Fields().Get(i)
		idx := GetIndexOptions(fd, t.indexExt)
		for _, one := range idx {
			if one.Name == "" {
				continue
			}
			if _, ok := t.byIndex[one.Name]; !ok {
				t.byIndex[one.Name] = map[string][]*dynamicpb.Message{}
			}
		}
	}

	return t, nil
}

// LoadFromBytes 反序列化 table pb 二进制并构建主键/索引。
func (t *Table) LoadFromBytes(b []byte) error {
	t.mu.Lock()
	defer t.mu.Unlock()

	msg := dynamicpb.NewMessage(t.TableDesc)
	if err := proto.Unmarshal(b, msg); err != nil {
		return fmt.Errorf("unmarshal table: %w", err)
	}

	rowsField := t.TableDesc.Fields().ByName("rows")
	list := msg.Get(rowsField).List()

	rows := make([]*dynamicpb.Message, 0, list.Len())
	byPK := make(map[string]*dynamicpb.Message, list.Len())

	byIndex := map[string]map[string][]*dynamicpb.Message{}
	for name := range t.byIndex {
		byIndex[name] = map[string][]*dynamicpb.Message{}
	}

	for i := 0; i < list.Len(); i++ {
		row := list.Get(i).Message()
		dm, ok := row.(*dynamicpb.Message)
		if !ok {
			// 理论上 dynamicpb 反序列化后应当就是 *dynamicpb.Message
			dm = dynamicpb.NewMessage(t.RowDesc)
			proto.Merge(dm, row.Interface())
		}
		rows = append(rows, dm)

		pkKey, err := encodeScalarKey(dm.ProtoReflect().Get(t.pkField), t.pkField)
		if err != nil {
			return fmt.Errorf("row[%d] primary key encode: %w", i, err)
		}
		if pkKey == "" {
			return fmt.Errorf("row[%d] primary key is empty", i)
		}
		if _, exists := byPK[pkKey]; exists {
			return fmt.Errorf("duplicate primary key: %q", pkKey)
		}
		byPK[pkKey] = dm

		// build indexes
		for idxName := range byIndex {
			fd := fieldByIndexName(t.RowDesc, idxName, t.indexExt)
			if fd == nil {
				continue
			}
			key, err := encodeScalarKey(dm.ProtoReflect().Get(fd), fd)
			if err != nil {
				return fmt.Errorf("row[%d] index %s encode: %w", i, idxName, err)
			}
			byIndex[idxName][key] = append(byIndex[idxName][key], dm)
		}
	}

	// enforce unique indexes
	for i := 0; i < t.RowDesc.Fields().Len(); i++ {
		fd := t.RowDesc.Fields().Get(i)
		for _, idx := range GetIndexOptions(fd, t.indexExt) {
			if !idx.Unique {
				continue
			}
			m := byIndex[idx.Name]
			for k, v := range m {
				if len(v) > 1 {
					return fmt.Errorf("unique index %s conflict on key %q", idx.Name, k)
				}
			}
		}
	}

	t.Rows = rows
	t.byPK = byPK
	t.byIndex = byIndex
	return nil
}

func (t *Table) All() []*dynamicpb.Message {
	t.mu.RLock()
	defer t.mu.RUnlock()
	out := make([]*dynamicpb.Message, len(t.Rows))
	copy(out, t.Rows)
	return out
}

func (t *Table) GetByPK(v any) (*dynamicpb.Message, bool, error) {
	t.mu.RLock()
	defer t.mu.RUnlock()

	key, err := encodeAnyKey(v, t.pkField)
	if err != nil {
		return nil, false, err
	}
	row, ok := t.byPK[key]
	return row, ok, nil
}

func (t *Table) FindByIndex(indexName string, v any) ([]*dynamicpb.Message, error) {
	t.mu.RLock()
	defer t.mu.RUnlock()

	idxMap, ok := t.byIndex[indexName]
	if !ok {
		return nil, fmt.Errorf("unknown index: %s", indexName)
	}
	fd := fieldByIndexName(t.RowDesc, indexName, t.indexExt)
	if fd == nil {
		return nil, fmt.Errorf("index %s not bound to any field", indexName)
	}
	key, err := encodeAnyKey(v, fd)
	if err != nil {
		return nil, err
	}
	rows := idxMap[key]
	out := make([]*dynamicpb.Message, len(rows))
	copy(out, rows)
	return out, nil
}

func fieldByIndexName(md protoreflect.MessageDescriptor, indexName string, indexExt protoreflect.ExtensionType) protoreflect.FieldDescriptor {
	for i := 0; i < md.Fields().Len(); i++ {
		fd := md.Fields().Get(i)
		for _, idx := range GetIndexOptions(fd, indexExt) {
			if idx.Name == indexName {
				return fd
			}
		}
	}
	return nil
}

func encodeAnyKey(v any, fd protoreflect.FieldDescriptor) (string, error) {
	switch x := v.(type) {
	case string:
		return x, nil
	case int:
		return strconv.FormatInt(int64(x), 10), nil
	case int32:
		return strconv.FormatInt(int64(x), 10), nil
	case int64:
		return strconv.FormatInt(x, 10), nil
	case uint32:
		return strconv.FormatUint(uint64(x), 10), nil
	case uint64:
		return strconv.FormatUint(x, 10), nil
	case bool:
		if x {
			return "1", nil
		}
		return "0", nil
	default:
		return "", fmt.Errorf("unsupported key type %T for field %s", v, fd.FullName())
	}
}

func encodeScalarKey(v protoreflect.Value, fd protoreflect.FieldDescriptor) (string, error) {
	if fd.IsList() || fd.IsMap() {
		return "", fmt.Errorf("list/map key not supported: %s", fd.FullName())
	}
	switch fd.Kind() {
	case protoreflect.StringKind:
		return v.String(), nil
	case protoreflect.BoolKind:
		if v.Bool() {
			return "1", nil
		}
		return "0", nil
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		return strconv.FormatInt(int64(v.Int()), 10), nil
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		return strconv.FormatInt(v.Int(), 10), nil
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		return strconv.FormatUint(uint64(v.Uint()), 10), nil
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		return strconv.FormatUint(v.Uint(), 10), nil
	case protoreflect.EnumKind:
		return strconv.FormatInt(int64(v.Enum()), 10), nil
	default:
		return "", fmt.Errorf("unsupported key kind %s for %s", fd.Kind(), fd.FullName())
	}
}

func findMessage(files *protoregistry.Files, fullName protoreflect.FullName) (protoreflect.MessageDescriptor, error) {
	var out protoreflect.MessageDescriptor
	files.RangeFiles(func(fd protoreflect.FileDescriptor) bool {
		if md := findNestedMessage(fd.Messages(), fullName); md != nil {
			out = md
			return false
		}
		return true
	})
	if out == nil {
		return nil, fmt.Errorf("message not found: %s", fullName)
	}
	return out, nil
}

func findNestedMessage(mds protoreflect.MessageDescriptors, fullName protoreflect.FullName) protoreflect.MessageDescriptor {
	for i := 0; i < mds.Len(); i++ {
		md := mds.Get(i)
		if md.FullName() == fullName {
			return md
		}
		if nested := findNestedMessage(md.Messages(), fullName); nested != nil {
			return nested
		}
	}
	return nil
}

func findExtension(files *protoregistry.Files, fullName string) protoreflect.ExtensionDescriptor {
	var out protoreflect.ExtensionDescriptor
	files.RangeFiles(func(fd protoreflect.FileDescriptor) bool {
		for i := 0; i < fd.Extensions().Len(); i++ {
			ed := fd.Extensions().Get(i)
			if string(ed.FullName()) == fullName {
				out = ed
				return false
			}
		}
		for i := 0; i < fd.Messages().Len(); i++ {
			if ed := findExtensionInMessage(fd.Messages().Get(i), fullName); ed != nil {
				out = ed
				return false
			}
		}
		return true
	})
	return out
}

func findExtensionInMessage(md protoreflect.MessageDescriptor, fullName string) protoreflect.ExtensionDescriptor {
	for i := 0; i < md.Extensions().Len(); i++ {
		ed := md.Extensions().Get(i)
		if string(ed.FullName()) == fullName {
			return ed
		}
	}
	for i := 0; i < md.Messages().Len(); i++ {
		if ed := findExtensionInMessage(md.Messages().Get(i), fullName); ed != nil {
			return ed
		}
	}
	return nil
}
