package resource

import (
	"fmt"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/dynamicpb"
)

type IndexOpt struct {
	Name   string
	Unique bool
}

func findPrimaryKeyField(md protoreflect.MessageDescriptor, primaryKeyExt protoreflect.ExtensionType) (protoreflect.FieldDescriptor, error) {
	var pk protoreflect.FieldDescriptor
	for i := 0; i < md.Fields().Len(); i++ {
		fd := md.Fields().Get(i)
		if IsPrimaryKey(fd, primaryKeyExt) {
			if pk != nil {
				return nil, fmt.Errorf("multiple primary keys in %s", md.FullName())
			}
			pk = fd
		}
	}
	if pk == nil {
		return nil, fmt.Errorf("missing primary key in %s", md.FullName())
	}
	return pk, nil
}

func IsPrimaryKey(fd protoreflect.FieldDescriptor, primaryKeyExt protoreflect.ExtensionType) bool {
	if primaryKeyExt == nil {
		return false
	}
	opts, ok := fd.Options().(proto.Message)
	if !ok || opts == nil {
		return false
	}
	if !proto.HasExtension(opts, primaryKeyExt) {
		return false
	}
	v := proto.GetExtension(opts, primaryKeyExt)
	b, ok := v.(bool)
	return ok && b
}

func GetIndexOptions(fd protoreflect.FieldDescriptor, indexExt protoreflect.ExtensionType) []IndexOpt {
	if indexExt == nil {
		return nil
	}
	opts, ok := fd.Options().(proto.Message)
	if !ok || opts == nil {
		return nil
	}
	if !proto.HasExtension(opts, indexExt) {
		return nil
	}

	v := proto.GetExtension(opts, indexExt)
	msgs := asProtoMessages(v)
	out := make([]IndexOpt, 0, len(msgs))
	for _, pm := range msgs {
		m := pm.ProtoReflect()
		nameFd := m.Descriptor().Fields().ByName("name")
		uniqFd := m.Descriptor().Fields().ByName("unique")
		var name string
		var uniq bool
		if nameFd != nil {
			name = m.Get(nameFd).String()
		}
		if uniqFd != nil {
			uniq = m.Get(uniqFd).Bool()
		}
		out = append(out, IndexOpt{Name: name, Unique: uniq})
	}
	return out
}

// asProtoMessages 适配 repeated message 扩展字段的返回类型（dynamic extension 常见为 []*dynamicpb.Message）。
func asProtoMessages(v any) []proto.Message {
	switch x := v.(type) {
	case protoreflect.List:
		out := make([]proto.Message, 0, x.Len())
		for i := 0; i < x.Len(); i++ {
			m := x.Get(i).Message()
			if pm, ok := m.Interface().(proto.Message); ok {
				out = append(out, pm)
			}
		}
		return out
	case []*dynamicpb.Message:
		out := make([]proto.Message, 0, len(x))
		for _, m := range x {
			out = append(out, m)
		}
		return out
	case []proto.Message:
		return x
	case []any:
		out := make([]proto.Message, 0, len(x))
		for _, it := range x {
			if pm, ok := it.(proto.Message); ok {
				out = append(out, pm)
			}
		}
		return out
	default:
		return nil
	}
}
