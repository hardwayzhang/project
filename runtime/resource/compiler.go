package resource

import (
	"context"
	"fmt"
	"sort"

	"github.com/bufbuild/protocompile"
	"github.com/bufbuild/protocompile/linker"
	"github.com/bufbuild/protocompile/protoutil"
	"google.golang.org/protobuf/types/descriptorpb"
)

// CompileFiles 在运行期编译 .proto（无需安装 protoc），返回 linker.Files。
func CompileFiles(protoDir string, entryFiles ...string) (linker.Files, error) {
	resolver := protocompile.WithStandardImports(&protocompile.SourceResolver{ImportPaths: []string{protoDir, "."}})
	compiler := protocompile.Compiler{Resolver: resolver}

	files, err := compiler.Compile(context.Background(), entryFiles...)
	if err != nil {
		return nil, fmt.Errorf("compile protos: %w", err)
	}

	// 返回入口 + 全部传递依赖（方便后续查找自定义 option 的 extension descriptor）
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
			visit(f.FindImportByPath(imp))
		}
	}
	for _, f := range files {
		visit(f)
	}

	out := make(linker.Files, 0, len(seen))
	for _, f := range seen {
		out = append(out, f)
	}
	return out, nil
}

// CompileDescriptorSet 在运行期编译 .proto（无需安装 protoc），返回 FileDescriptorSet。
// entryFiles 是要编译的入口 proto（相对 protoDir 或工作目录均可，取决于 import path 配置）。
func CompileDescriptorSet(protoDir string, entryFiles ...string) (*descriptorpb.FileDescriptorSet, error) {
	files, err := CompileFiles(protoDir, entryFiles...)
	if err != nil {
		return nil, err
	}

	// 收集编译入口 + 全部传递依赖（否则 protodesc.NewFiles 会报 import not found）
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

	out := &descriptorpb.FileDescriptorSet{}
	for _, f := range seen {
		out.File = append(out.File, protoutil.ProtoFromFileDescriptor(f))
	}

	sort.Slice(out.File, func(i, j int) bool { return out.File[i].GetName() < out.File[j].GetName() })
	return out, nil
}
