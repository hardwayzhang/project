package resource

import (
	"fmt"

	"github.com/bufbuild/protocompile/linker"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/dynamicpb"
)

// NewTableFromFiles 直接基于 protocompile/linker 的编译结果初始化 Table（无需 protodesc.NewFiles）。
func NewTableFromFiles(files linker.Files, tableMessageFullName string) (*Table, error) {
	md, err := findMessageInLinkerFiles(files, protoreflect.FullName(tableMessageFullName))
	if err != nil {
		return nil, err
	}

	rowsField := md.Fields().ByName("rows")
	if rowsField == nil || !rowsField.IsList() || rowsField.Kind() != protoreflect.MessageKind {
		return nil, fmt.Errorf("%s must have repeated message field named 'rows'", tableMessageFullName)
	}
	rowDesc := rowsField.Message()

	pkExtDesc := findExtensionInLinkerFiles(files, protoreflect.FullName("resource.options.primary_key"))
	idxExtDesc := findExtensionInLinkerFiles(files, protoreflect.FullName("resource.options.index"))

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

func findMessageInLinkerFiles(files linker.Files, fullName protoreflect.FullName) (protoreflect.MessageDescriptor, error) {
	for _, f := range files {
		if md := findMessageInFile(f, fullName); md != nil {
			return md, nil
		}
	}
	return nil, fmt.Errorf("message not found: %s", fullName)
}

func findMessageInFile(fd protoreflect.FileDescriptor, fullName protoreflect.FullName) protoreflect.MessageDescriptor {
	for i := 0; i < fd.Messages().Len(); i++ {
		md := fd.Messages().Get(i)
		if md.FullName() == fullName {
			return md
		}
		if nested := findMessageInNested(md, fullName); nested != nil {
			return nested
		}
	}
	return nil
}

func findMessageInNested(md protoreflect.MessageDescriptor, fullName protoreflect.FullName) protoreflect.MessageDescriptor {
	for i := 0; i < md.Messages().Len(); i++ {
		child := md.Messages().Get(i)
		if child.FullName() == fullName {
			return child
		}
		if nested := findMessageInNested(child, fullName); nested != nil {
			return nested
		}
	}
	return nil
}

func findExtensionInLinkerFiles(files linker.Files, fullName protoreflect.FullName) protoreflect.ExtensionDescriptor {
	for _, f := range files {
		for i := 0; i < f.Extensions().Len(); i++ {
			ed := f.Extensions().Get(i)
			if ed.FullName() == fullName {
				return ed
			}
		}
		for i := 0; i < f.Messages().Len(); i++ {
			if ed := findExtensionInMessageDesc(f.Messages().Get(i), fullName); ed != nil {
				return ed
			}
		}
	}
	return nil
}

func findExtensionInMessageDesc(md protoreflect.MessageDescriptor, fullName protoreflect.FullName) protoreflect.ExtensionDescriptor {
	for i := 0; i < md.Extensions().Len(); i++ {
		ed := md.Extensions().Get(i)
		if ed.FullName() == fullName {
			return ed
		}
	}
	for i := 0; i < md.Messages().Len(); i++ {
		if ed := findExtensionInMessageDesc(md.Messages().Get(i), fullName); ed != nil {
			return ed
		}
	}
	return nil
}
