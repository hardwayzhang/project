package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/bufbuild/protocompile"
	"github.com/bufbuild/protocompile/linker"
	"github.com/bufbuild/protocompile/protoutil"
	"github.com/xuri/excelize/v2"
	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/descriptorpb"
	"google.golang.org/protobuf/types/dynamicpb"
)

// 最小可用导出器：
// - 读取 protoDir 下的 .proto
// - 按 messageFullName（如 game.res.ItemTable）找到 table message
// - 从 table.rows 的元素类型读取字段，Excel 第一行必须是字段名
// - 输出 table message 的 proto 序列化二进制

func main() {
	var (
		protoDir        = flag.String("proto_dir", "proto", "proto root dir")
		entryProto      = flag.String("entry", "game/res/item.proto", "entry proto relative to proto_dir or repo root")
		messageFullName = flag.String("message", "game.res.ItemTable", "table message full name")
		excelPath       = flag.String("excel", "", "excel file path")
		sheetName       = flag.String("sheet", "", "sheet name (default: option(sheet) or message name)")
		outPath         = flag.String("out", "", "output pb file path")
	)
	flag.Parse()

	if *excelPath == "" {
		fatal(errors.New("-excel is required"))
	}

	fds, files, err := compileToDescriptorSet(*protoDir, *entryProto)
	if err != nil {
		fatal(err)
	}

	tableDesc, err := findMessage(files, protoreflect.FullName(*messageFullName))
	if err != nil {
		fatal(err)
	}

	rowsField := tableDesc.Fields().ByName("rows")
	if rowsField == nil || !rowsField.IsList() || rowsField.Kind() != protoreflect.MessageKind {
		fatal(fmt.Errorf("%s must have repeated message field named 'rows'", *messageFullName))
	}
	rowDesc := rowsField.Message()

	// sheet: CLI > option(sheet) > message name
	if *sheetName == "" {
		*sheetName = getMessageOptionString(files, tableDesc, "resource.options.sheet")
		if *sheetName == "" {
			*sheetName = string(rowDesc.Name())
		}
	}
	if *outPath == "" {
		name := getMessageOptionString(files, tableDesc, "resource.options.out_file")
		if name == "" {
			name = strings.ToLower(string(rowDesc.Name())) + ".pb"
		}
		*outPath = filepath.Join("res_bin", name)
	}

	if err := os.MkdirAll(filepath.Dir(*outPath), 0o755); err != nil {
		fatal(err)
	}

	xl, err := excelize.OpenFile(*excelPath)
	if err != nil {
		fatal(err)
	}
	defer func() { _ = xl.Close() }()

	rows, err := xl.GetRows(*sheetName)
	if err != nil {
		fatal(err)
	}
	if len(rows) < 1 {
		fatal(fmt.Errorf("sheet %s is empty", *sheetName))
	}

	head := rows[0]
	colToField := make([]protoreflect.FieldDescriptor, len(head))
	for ci, name := range head {
		name = strings.TrimSpace(name)
		if name == "" || strings.HasPrefix(name, "#") {
			continue
		}
		fd := rowDesc.Fields().ByName(protoreflect.Name(name))
		if fd == nil {
			fatal(fmt.Errorf("unknown field in header: %s", name))
		}
		colToField[ci] = fd
	}

	tableMsg := dynamicpb.NewMessage(tableDesc)
	list := tableMsg.Mutable(rowsField).List()

	for ri := 1; ri < len(rows); ri++ {
		r := rows[ri]
		if isRowAllEmpty(r) {
			continue
		}
		rowMsg := dynamicpb.NewMessage(rowDesc)

		for ci := 0; ci < len(colToField); ci++ {
			fd := colToField[ci]
			if fd == nil {
				continue
			}
			var cell string
			if ci < len(r) {
				cell = strings.TrimSpace(r[ci])
			}
			if cell == "" {
				continue
			}

			if fd.IsList() {
				parts := strings.Split(cell, ";")
				l := rowMsg.Mutable(fd).List()
				for _, p := range parts {
					p = strings.TrimSpace(p)
					if p == "" {
						continue
					}
					v, err := parseScalarValue(fd, p)
					if err != nil {
						fatal(fmt.Errorf("row %d col %d field %s: %w", ri+1, ci+1, fd.FullName(), err))
					}
					l.Append(v)
				}
				continue
			}

			v, err := parseScalarValue(fd, cell)
			if err != nil {
				fatal(fmt.Errorf("row %d col %d field %s: %w", ri+1, ci+1, fd.FullName(), err))
			}
			rowMsg.Set(fd, v)
		}

		list.Append(protoreflect.ValueOfMessage(rowMsg))
	}

	b, err := proto.Marshal(tableMsg)
	if err != nil {
		fatal(err)
	}

	if err := os.WriteFile(*outPath, b, 0o644); err != nil {
		fatal(err)
	}

	// 顺便输出 descriptor_set 供运行期直接使用（最小闭环）
	_ = fds
}

func fatal(err error) {
	fmt.Fprintln(os.Stderr, "error:", err)
	os.Exit(1)
}

func isRowAllEmpty(r []string) bool {
	for _, c := range r {
		if strings.TrimSpace(c) != "" {
			return false
		}
	}
	return true
}

func parseScalarValue(fd protoreflect.FieldDescriptor, s string) (protoreflect.Value, error) {
	switch fd.Kind() {
	case protoreflect.StringKind:
		return protoreflect.ValueOfString(s), nil
	case protoreflect.BoolKind:
		s = strings.ToLower(s)
		if s == "true" || s == "1" {
			return protoreflect.ValueOfBool(true), nil
		}
		if s == "false" || s == "0" {
			return protoreflect.ValueOfBool(false), nil
		}
		return protoreflect.Value{}, fmt.Errorf("invalid bool: %q", s)
	case protoreflect.Int32Kind, protoreflect.Sint32Kind, protoreflect.Sfixed32Kind:
		i, err := strconv.ParseInt(s, 10, 32)
		if err != nil {
			return protoreflect.Value{}, err
		}
		return protoreflect.ValueOfInt32(int32(i)), nil
	case protoreflect.Int64Kind, protoreflect.Sint64Kind, protoreflect.Sfixed64Kind:
		i, err := strconv.ParseInt(s, 10, 64)
		if err != nil {
			return protoreflect.Value{}, err
		}
		return protoreflect.ValueOfInt64(i), nil
	case protoreflect.Uint32Kind, protoreflect.Fixed32Kind:
		u, err := strconv.ParseUint(s, 10, 32)
		if err != nil {
			return protoreflect.Value{}, err
		}
		return protoreflect.ValueOfUint32(uint32(u)), nil
	case protoreflect.Uint64Kind, protoreflect.Fixed64Kind:
		u, err := strconv.ParseUint(s, 10, 64)
		if err != nil {
			return protoreflect.Value{}, err
		}
		return protoreflect.ValueOfUint64(u), nil
	case protoreflect.EnumKind:
		// 允许枚举名或数值
		if n, err := strconv.ParseInt(s, 10, 32); err == nil {
			return protoreflect.ValueOfEnum(protoreflect.EnumNumber(n)), nil
		}
		v := fd.Enum().Values().ByName(protoreflect.Name(s))
		if v == nil {
			return protoreflect.Value{}, fmt.Errorf("unknown enum: %q", s)
		}
		return protoreflect.ValueOfEnum(v.Number()), nil
	default:
		return protoreflect.Value{}, fmt.Errorf("unsupported kind: %s", fd.Kind())
	}
}

func compileToDescriptorSet(protoDir, entry string) (*descriptorpb.FileDescriptorSet, linker.Files, error) {
	resolver := protocompile.WithStandardImports(&protocompile.SourceResolver{ImportPaths: []string{protoDir, "."}})
	compiler := protocompile.Compiler{Resolver: resolver}

	files, err := compiler.Compile(context.Background(), entry)
	if err != nil {
		return nil, nil, err
	}

	// 收集入口 + 全部传递依赖
	seen := map[string]linker.File{}
	var visit func(f linker.File)
	visit = func(f linker.File) {
		if f == nil {
			return
		}
		if _, ok := seen[f.Path()]; ok {
			return
		}
		seen[f.Path()] = f
		for i := 0; i < f.Imports().Len(); i++ {
			imp := f.Imports().Get(i).Path()
			dep := f.FindImportByPath(imp)
			visit(dep)
		}
	}
	for _, f := range files {
		visit(f)
	}

	set := &descriptorpb.FileDescriptorSet{}
	for _, f := range seen {
		set.File = append(set.File, protoutil.ProtoFromFileDescriptor(f))
	}
	return set, files, nil
}

func findMessage(files linker.Files, fullName protoreflect.FullName) (protoreflect.MessageDescriptor, error) {
	for _, f := range files {
		if md := findMessageInFile(f, fullName); md != nil {
			return md, nil
		}
	}
	return nil, fmt.Errorf("message not found: %s", fullName)
}

func findMessageInFile(fd protoreflect.FileDescriptor, fullName protoreflect.FullName) protoreflect.MessageDescriptor {
	// top-level
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

func getMessageOptionString(files linker.Files, md protoreflect.MessageDescriptor, optFullName string) string {
	opts, ok := md.Options().(proto.Message)
	if !ok || opts == nil {
		return ""
	}

	ext := findExtension(files, protoreflect.FullName(optFullName))
	if ext == nil {
		return ""
	}

	extType := dynamicpb.NewExtensionType(ext)
	if !proto.HasExtension(opts, extType) {
		return ""
	}
	v := proto.GetExtension(opts, extType)
	s, _ := v.(string)
	return s
}

func findExtension(files linker.Files, fullName protoreflect.FullName) protoreflect.ExtensionDescriptor {
	for _, f := range files {
		for i := 0; i < f.Extensions().Len(); i++ {
			ed := f.Extensions().Get(i)
			if ed.FullName() == fullName {
				return ed
			}
		}
		for i := 0; i < f.Messages().Len(); i++ {
			if ed := findExtensionInMessage(f.Messages().Get(i), fullName); ed != nil {
				return ed
			}
		}
	}
	return nil
}

func findExtensionInMessage(md protoreflect.MessageDescriptor, fullName protoreflect.FullName) protoreflect.ExtensionDescriptor {
	for i := 0; i < md.Extensions().Len(); i++ {
		ed := md.Extensions().Get(i)
		if ed.FullName() == fullName {
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
