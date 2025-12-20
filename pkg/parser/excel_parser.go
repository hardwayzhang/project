// Package parser 提供Excel文件解析功能
package parser

import (
	"fmt"
	"strconv"
	"strings"

	"github.com/xuri/excelize/v2"
)

// ExcelParser Excel文件解析器
type ExcelParser struct {
	file      *excelize.File
	sheetName string
	headers   []string
	rows      [][]string
}

// NewExcelParser 创建新的Excel解析器
func NewExcelParser(filePath string, sheetName string) (*ExcelParser, error) {
	f, err := excelize.OpenFile(filePath)
	if err != nil {
		return nil, fmt.Errorf("failed to open excel file: %w", err)
	}

	if sheetName == "" {
		sheetName = f.GetSheetName(0)
	}

	rows, err := f.GetRows(sheetName)
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("failed to get rows: %w", err)
	}

	if len(rows) < 1 {
		f.Close()
		return nil, fmt.Errorf("excel file has no data")
	}

	return &ExcelParser{
		file:      f,
		sheetName: sheetName,
		headers:   rows[0],
		rows:      rows[1:],
	}, nil
}

// Close 关闭Excel文件
func (p *ExcelParser) Close() error {
	return p.file.Close()
}

// GetHeaders 获取表头(第一行)
func (p *ExcelParser) GetHeaders() []string {
	return p.headers
}

// GetRows 获取数据行(不含表头)
func (p *ExcelParser) GetRows() [][]string {
	return p.rows
}

// GetRowCount 获取数据行数
func (p *ExcelParser) GetRowCount() int {
	return len(p.rows)
}

// GetColumnIndex 根据列名获取列索引
func (p *ExcelParser) GetColumnIndex(columnName string) int {
	for i, h := range p.headers {
		if h == columnName {
			return i
		}
	}
	return -1
}

// GetCellValue 获取指定行列的值
func (p *ExcelParser) GetCellValue(rowIndex int, columnName string) (string, error) {
	colIndex := p.GetColumnIndex(columnName)
	if colIndex < 0 {
		return "", fmt.Errorf("column %s not found", columnName)
	}
	if rowIndex >= len(p.rows) {
		return "", fmt.Errorf("row index %d out of range", rowIndex)
	}
	if colIndex >= len(p.rows[rowIndex]) {
		return "", nil // 空值
	}
	return p.rows[rowIndex][colIndex], nil
}

// RowData 表示一行数据的映射
type RowData map[string]string

// GetRowAsMap 将指定行转换为map
func (p *ExcelParser) GetRowAsMap(rowIndex int) (RowData, error) {
	if rowIndex >= len(p.rows) {
		return nil, fmt.Errorf("row index %d out of range", rowIndex)
	}

	data := make(RowData)
	row := p.rows[rowIndex]
	for i, h := range p.headers {
		if i < len(row) {
			data[h] = row[i]
		} else {
			data[h] = ""
		}
	}
	return data, nil
}

// ParseInt 解析整数
func ParseInt(s string) (int64, error) {
	if s == "" {
		return 0, nil
	}
	return strconv.ParseInt(strings.TrimSpace(s), 10, 64)
}

// ParseFloat 解析浮点数
func ParseFloat(s string) (float64, error) {
	if s == "" {
		return 0, nil
	}
	return strconv.ParseFloat(strings.TrimSpace(s), 64)
}

// ParseBool 解析布尔值
func ParseBool(s string) (bool, error) {
	if s == "" {
		return false, nil
	}
	s = strings.ToLower(strings.TrimSpace(s))
	switch s {
	case "true", "1", "yes", "是":
		return true, nil
	case "false", "0", "no", "否", "":
		return false, nil
	default:
		return false, fmt.Errorf("invalid bool value: %s", s)
	}
}

// ParseIntArray 解析整数数组
func ParseIntArray(s string, separator string) ([]int64, error) {
	if s == "" {
		return nil, nil
	}
	if separator == "" {
		separator = ","
	}
	parts := strings.Split(s, separator)
	result := make([]int64, 0, len(parts))
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p == "" {
			continue
		}
		v, err := strconv.ParseInt(p, 10, 64)
		if err != nil {
			return nil, fmt.Errorf("failed to parse int: %w", err)
		}
		result = append(result, v)
	}
	return result, nil
}

// ParseStringArray 解析字符串数组
func ParseStringArray(s string, separator string) []string {
	if s == "" {
		return nil
	}
	if separator == "" {
		separator = ","
	}
	parts := strings.Split(s, separator)
	result := make([]string, 0, len(parts))
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p != "" {
			result = append(result, p)
		}
	}
	return result
}

// ParseNestedField 解析嵌套字段 (格式: "field1.subfield1=value1;field1.subfield2=value2")
func ParseNestedField(s string, separator string) (map[string]string, error) {
	if s == "" {
		return nil, nil
	}
	if separator == "" {
		separator = ";"
	}

	result := make(map[string]string)
	parts := strings.Split(s, separator)
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p == "" {
			continue
		}
		kv := strings.SplitN(p, "=", 2)
		if len(kv) != 2 {
			return nil, fmt.Errorf("invalid nested field format: %s", p)
		}
		result[strings.TrimSpace(kv[0])] = strings.TrimSpace(kv[1])
	}
	return result, nil
}
