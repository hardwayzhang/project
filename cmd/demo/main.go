package main

import (
	"fmt"
	"os"

	"example.com/res/runtime/resource"
)

func main() {
	// 1) 编译 proto（无需 protoc）
	files, err := resource.CompileFiles("proto", "game/res/item.proto")
	if err != nil {
		panic(err)
	}

	// 2) 创建资源表（基于主键/索引 option 自动识别）
	t, err := resource.NewTableFromFiles(files, "game.res.ItemTable")
	if err != nil {
		panic(err)
	}

	// 3) 加载导出的 pb
	b, err := os.ReadFile("res_bin/item.pb")
	if err != nil {
		panic(err)
	}
	if err := t.LoadFromBytes(b); err != nil {
		panic(err)
	}

	// 4) 主键访问
	row, ok, err := t.GetByPK(int32(2))
	if err != nil {
		panic(err)
	}
	fmt.Println("GetByPK(2) ok=", ok)
	if ok {
		fmt.Println(row)
	}

	// 5) 索引访问（type=2）
	rows, err := t.FindByIndex("type", int32(2))
	if err != nil {
		panic(err)
	}
	fmt.Println("FindByIndex(type=2) size=", len(rows))
	for _, r := range rows {
		fmt.Println(" ", r)
	}
}
