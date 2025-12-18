package main

import (
	"os"
	"path/filepath"

	"github.com/xuri/excelize/v2"
)

func main() {
	_ = os.MkdirAll("res_excel", 0o755)

	xl := excelize.NewFile()
	sheet := "Item"
	idx, _ := xl.NewSheet(sheet)
	xl.SetActiveSheet(idx)

	// header: 必须是 proto 字段名
	head := []string{"id", "name", "type", "quality"}
	for i, h := range head {
		cell, _ := excelize.CoordinatesToCellName(i+1, 1)
		_ = xl.SetCellStr(sheet, cell, h)
	}

	rows := [][]any{
		{1, "Wood Sword", 1, 1},
		{2, "Iron Sword", 1, 2},
		{3, "Health Potion", 2, 1},
		{4, "Mana Potion", 2, 1},
	}
	for r, row := range rows {
		for c, v := range row {
			cell, _ := excelize.CoordinatesToCellName(c+1, r+2)
			_ = xl.SetCellValue(sheet, cell, v)
		}
	}

	out := filepath.Join("res_excel", "item.xlsx")
	_ = xl.SaveAs(out)
	_ = xl.Close()
}
