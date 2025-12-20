// excel2pb - Excel到Protobuf二进制转换工具
// 读取Excel文件,根据proto定义转换为protobuf二进制格式

package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"

	"github.com/example/gores/pkg/parser"
	"google.golang.org/protobuf/encoding/protojson"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protodesc"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/reflect/protoregistry"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/dynamicpb"
)

var (
	excelDir     = flag.String("excel", "./excel", "Excel files directory")
	outputDir    = flag.String("output", "./data", "Output directory for binary files")
	protoDescSet = flag.String("descriptor", "", "Proto descriptor set file path")
	messageName  = flag.String("message", "", "Full message name (e.g., game.ItemConfig)")
	sheetName    = flag.String("sheet", "", "Excel sheet name (default: first sheet)")
	format       = flag.String("format", "binary", "Output format: binary, json")
)

func main() {
	flag.Parse()

	if *protoDescSet == "" {
		log.Fatal("descriptor set file is required")
	}

	if *messageName == "" {
		log.Fatal("message name is required")
	}

	// 加载descriptor set
	descSetData, err := os.ReadFile(*protoDescSet)
	if err != nil {
		log.Fatalf("failed to read descriptor set: %v", err)
	}

	var descSet descriptorpb.FileDescriptorSet
	if err := proto.Unmarshal(descSetData, &descSet); err != nil {
		log.Fatalf("failed to unmarshal descriptor set: %v", err)
	}

	// 构建文件注册表
	files, err := protodesc.NewFiles(&descSet)
	if err != nil {
		log.Fatalf("failed to create file registry: %v", err)
	}

	// 查找消息类型
	msgDesc, err := files.FindDescriptorByName(protoreflect.FullName(*messageName))
	if err != nil {
		log.Fatalf("failed to find message %s: %v", *messageName, err)
	}

	md, ok := msgDesc.(protoreflect.MessageDescriptor)
	if !ok {
		log.Fatalf("%s is not a message type", *messageName)
	}

	// 获取资源配置选项
	resOpts := getResourceOptions(md)
	excelFile := resOpts.excelFile
	if excelFile == "" {
		excelFile = string(md.Name())
	}

	sheet := *sheetName
	if sheet == "" && resOpts.sheetName != "" {
		sheet = resOpts.sheetName
	}

	// 解析Excel文件
	excelPath := filepath.Join(*excelDir, excelFile+".xlsx")
	p, err := parser.NewExcelParser(excelPath, sheet)
	if err != nil {
		log.Fatalf("failed to parse excel: %v", err)
	}
	defer p.Close()

	// 转换数据
	messages, err := convertExcelToProto(p, md, files)
	if err != nil {
		log.Fatalf("failed to convert excel to proto: %v", err)
	}

	// 输出
	if err := os.MkdirAll(*outputDir, 0755); err != nil {
		log.Fatalf("failed to create output directory: %v", err)
	}

	outputPath := filepath.Join(*outputDir, excelFile)
	switch *format {
	case "binary":
		if err := writeBinary(outputPath+".bin", messages, md); err != nil {
			log.Fatalf("failed to write binary: %v", err)
		}
	case "json":
		if err := writeJSON(outputPath+".json", messages); err != nil {
			log.Fatalf("failed to write json: %v", err)
		}
	default:
		log.Fatalf("unknown format: %s", *format)
	}

	log.Printf("Successfully converted %d rows to %s", len(messages), outputPath)
}

// ResourceOptions 资源选项
type ResourceOptions struct {
	name        string
	excelFile   string
	sheetName   string
	hotReload   bool
	description string
}

// getResourceOptions 获取消息的资源选项
func getResourceOptions(md protoreflect.MessageDescriptor) ResourceOptions {
	opts := md.Options()
	if opts == nil {
		return ResourceOptions{}
	}

	// 尝试获取自定义选项
	// 注意:这里需要在编译时注册选项,动态获取需要特殊处理
	res := ResourceOptions{
		name: string(md.Name()),
	}

	// 从选项中提取信息(简化实现)
	return res
}

// FieldMeta 字段元信息
type FieldMeta struct {
	fieldDesc      protoreflect.FieldDescriptor
	isPrimaryKey   bool
	indexType      int
	indexName      string
	columnName     string
	arraySeparator string
}

// getFieldMeta 获取字段元信息
func getFieldMeta(fd protoreflect.FieldDescriptor) FieldMeta {
	meta := FieldMeta{
		fieldDesc:      fd,
		columnName:     string(fd.Name()),
		arraySeparator: ",",
	}

	// 可以从自定义选项中获取更多信息
	return meta
}

// convertExcelToProto 将Excel数据转换为Proto消息
func convertExcelToProto(p *parser.ExcelParser, md protoreflect.MessageDescriptor, files *protoregistry.Files) ([]proto.Message, error) {
	headers := p.GetHeaders()
	rows := p.GetRows()

	// 构建列名到字段的映射
	fieldMap := make(map[string]FieldMeta)
	for i := 0; i < md.Fields().Len(); i++ {
		fd := md.Fields().Get(i)
		meta := getFieldMeta(fd)
		fieldMap[meta.columnName] = meta
		// 同时支持使用字段名
		if string(fd.Name()) != meta.columnName {
			fieldMap[string(fd.Name())] = meta
		}
		// 支持json名
		if fd.JSONName() != meta.columnName {
			fieldMap[fd.JSONName()] = meta
		}
	}

	messages := make([]proto.Message, 0, len(rows))

	for rowIdx, row := range rows {
		msg := dynamicpb.NewMessage(md)

		for colIdx, header := range headers {
			if colIdx >= len(row) {
				continue
			}

			value := strings.TrimSpace(row[colIdx])
			if value == "" {
				continue
			}

			meta, ok := fieldMap[header]
			if !ok {
				// 尝试处理嵌套字段 (格式: parent.child)
				if strings.Contains(header, ".") {
					if err := setNestedField(msg, header, value, files); err != nil {
						log.Printf("warning: row %d, column %s: %v", rowIdx+2, header, err)
					}
				}
				continue
			}

			if err := setFieldValue(msg, meta, value, files); err != nil {
				return nil, fmt.Errorf("row %d, column %s: %w", rowIdx+2, header, err)
			}
		}

		messages = append(messages, msg)
	}

	return messages, nil
}

// setFieldValue 设置字段值
func setFieldValue(msg *dynamicpb.Message, meta FieldMeta, value string, files *protoregistry.Files) error {
	fd := meta.fieldDesc

	// 处理repeated字段
	if fd.IsList() {
		return setListFieldValue(msg, fd, value, meta.arraySeparator, files)
	}

	// 处理map字段
	if fd.IsMap() {
		return setMapFieldValue(msg, fd, value, files)
	}

	// 处理单个字段
	v, err := parseFieldValue(fd, value, files)
	if err != nil {
		return err
	}
	msg.Set(fd, v)
	return nil
}

// setListFieldValue 设置列表字段值
func setListFieldValue(msg *dynamicpb.Message, fd protoreflect.FieldDescriptor, value string, separator string, files *protoregistry.Files) error {
	if separator == "" {
		separator = ","
	}

	parts := strings.Split(value, separator)
	list := msg.Mutable(fd).List()

	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p == "" {
			continue
		}
		v, err := parseFieldValue(fd, p, files)
		if err != nil {
			return err
		}
		list.Append(v)
	}

	return nil
}

// setMapFieldValue 设置Map字段值
func setMapFieldValue(msg *dynamicpb.Message, fd protoreflect.FieldDescriptor, value string, files *protoregistry.Files) error {
	// 格式: key1:value1,key2:value2
	parts := strings.Split(value, ",")
	mapVal := msg.Mutable(fd).Map()
	keyFd := fd.MapKey()
	valFd := fd.MapValue()

	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p == "" {
			continue
		}
		kv := strings.SplitN(p, ":", 2)
		if len(kv) != 2 {
			return fmt.Errorf("invalid map format: %s", p)
		}

		key, err := parseFieldValue(keyFd, strings.TrimSpace(kv[0]), files)
		if err != nil {
			return fmt.Errorf("invalid map key: %w", err)
		}
		val, err := parseFieldValue(valFd, strings.TrimSpace(kv[1]), files)
		if err != nil {
			return fmt.Errorf("invalid map value: %w", err)
		}

		mapVal.Set(key.MapKey(), val)
	}

	return nil
}

// parseFieldValue 解析字段值
func parseFieldValue(fd protoreflect.FieldDescriptor, value string, files *protoregistry.Files) (protoreflect.Value, error) {
	switch fd.Kind() {
	case protoreflect.BoolKind:
		b, err := parser.ParseBool(value)
		if err != nil {
			return protoreflect.Value{}, err
		}
		return protoreflect.ValueOfBool(b), nil

	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		v, err := parser.ParseInt(value)
		if err != nil {
			return protoreflect.Value{}, err
		}
		return protoreflect.ValueOfInt32(int32(v)), nil

	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		v, err := parser.ParseInt(value)
		if err != nil {
			return protoreflect.Value{}, err
		}
		return protoreflect.ValueOfInt64(v), nil

	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		v, err := parser.ParseInt(value)
		if err != nil {
			return protoreflect.Value{}, err
		}
		return protoreflect.ValueOfUint32(uint32(v)), nil

	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		v, err := parser.ParseInt(value)
		if err != nil {
			return protoreflect.Value{}, err
		}
		return protoreflect.ValueOfUint64(uint64(v)), nil

	case protoreflect.FloatKind:
		v, err := parser.ParseFloat(value)
		if err != nil {
			return protoreflect.Value{}, err
		}
		return protoreflect.ValueOfFloat32(float32(v)), nil

	case protoreflect.DoubleKind:
		v, err := parser.ParseFloat(value)
		if err != nil {
			return protoreflect.Value{}, err
		}
		return protoreflect.ValueOfFloat64(v), nil

	case protoreflect.StringKind:
		return protoreflect.ValueOfString(value), nil

	case protoreflect.BytesKind:
		return protoreflect.ValueOfBytes([]byte(value)), nil

	case protoreflect.EnumKind:
		// 尝试按名称查找
		enumVal := fd.Enum().Values().ByName(protoreflect.Name(value))
		if enumVal != nil {
			return protoreflect.ValueOfEnum(enumVal.Number()), nil
		}
		// 尝试按数字查找
		v, err := parser.ParseInt(value)
		if err != nil {
			return protoreflect.Value{}, fmt.Errorf("invalid enum value: %s", value)
		}
		return protoreflect.ValueOfEnum(protoreflect.EnumNumber(v)), nil

	case protoreflect.MessageKind:
		// 嵌套消息使用JSON格式
		nestedMsg := dynamicpb.NewMessage(fd.Message())
		if err := protojson.Unmarshal([]byte(value), nestedMsg); err != nil {
			return protoreflect.Value{}, fmt.Errorf("invalid message json: %w", err)
		}
		return protoreflect.ValueOfMessage(nestedMsg), nil

	default:
		return protoreflect.Value{}, fmt.Errorf("unsupported field kind: %v", fd.Kind())
	}
}

// setNestedField 设置嵌套字段 (格式: parent.child.field)
func setNestedField(msg *dynamicpb.Message, path string, value string, files *protoregistry.Files) error {
	parts := strings.Split(path, ".")
	if len(parts) < 2 {
		return fmt.Errorf("invalid nested path: %s", path)
	}

	current := msg
	md := msg.Descriptor()

	// 遍历到倒数第二层
	for i := 0; i < len(parts)-1; i++ {
		fd := md.Fields().ByName(protoreflect.Name(parts[i]))
		if fd == nil {
			return fmt.Errorf("field %s not found in %s", parts[i], md.FullName())
		}

		if fd.Kind() != protoreflect.MessageKind {
			return fmt.Errorf("field %s is not a message type", parts[i])
		}

		// 获取或创建嵌套消息
		nestedMsg := current.Mutable(fd).Message()
		current = nestedMsg.(*dynamicpb.Message)
		md = fd.Message()
	}

	// 设置最终字段
	fieldName := parts[len(parts)-1]
	fd := md.Fields().ByName(protoreflect.Name(fieldName))
	if fd == nil {
		return fmt.Errorf("field %s not found in %s", fieldName, md.FullName())
	}

	meta := FieldMeta{fieldDesc: fd, arraySeparator: ","}
	return setFieldValue(current, meta, value, files)
}

// ResourceList 资源列表包装
type ResourceList struct {
	Items []proto.Message
}

// writeBinary 写入二进制文件
func writeBinary(path string, messages []proto.Message, md protoreflect.MessageDescriptor) error {
	// 创建一个包含所有消息的容器
	// 格式: [4字节数量][4字节长度1][数据1][4字节长度2][数据2]...
	data := make([]byte, 0)

	// 写入消息数量
	count := uint32(len(messages))
	data = append(data, byte(count), byte(count>>8), byte(count>>16), byte(count>>24))

	for _, msg := range messages {
		msgData, err := proto.Marshal(msg)
		if err != nil {
			return fmt.Errorf("failed to marshal message: %w", err)
		}

		// 写入消息长度
		length := uint32(len(msgData))
		data = append(data, byte(length), byte(length>>8), byte(length>>16), byte(length>>24))

		// 写入消息数据
		data = append(data, msgData...)
	}

	return os.WriteFile(path, data, 0644)
}

// writeJSON 写入JSON文件
func writeJSON(path string, messages []proto.Message) error {
	items := make([]json.RawMessage, len(messages))
	for i, msg := range messages {
		data, err := protojson.Marshal(msg)
		if err != nil {
			return fmt.Errorf("failed to marshal message to json: %w", err)
		}
		items[i] = data
	}

	output := struct {
		Items []json.RawMessage `json:"items"`
		Count int               `json:"count"`
	}{
		Items: items,
		Count: len(items),
	}

	data, err := json.MarshalIndent(output, "", "  ")
	if err != nil {
		return err
	}

	return os.WriteFile(path, data, 0644)
}
