package parser

import (
	"testing"
)

func TestParseInt(t *testing.T) {
	tests := []struct {
		input    string
		expected int64
		hasError bool
	}{
		{"123", 123, false},
		{"-456", -456, false},
		{"0", 0, false},
		{"", 0, false},
		{"  789  ", 789, false},
		{"abc", 0, true},
	}

	for _, tt := range tests {
		result, err := ParseInt(tt.input)
		if tt.hasError {
			if err == nil {
				t.Errorf("ParseInt(%q) expected error, got nil", tt.input)
			}
		} else {
			if err != nil {
				t.Errorf("ParseInt(%q) unexpected error: %v", tt.input, err)
			}
			if result != tt.expected {
				t.Errorf("ParseInt(%q) = %d, expected %d", tt.input, result, tt.expected)
			}
		}
	}
}

func TestParseFloat(t *testing.T) {
	tests := []struct {
		input    string
		expected float64
		hasError bool
	}{
		{"3.14", 3.14, false},
		{"-2.5", -2.5, false},
		{"0", 0, false},
		{"", 0, false},
		{"  1.5  ", 1.5, false},
		{"abc", 0, true},
	}

	for _, tt := range tests {
		result, err := ParseFloat(tt.input)
		if tt.hasError {
			if err == nil {
				t.Errorf("ParseFloat(%q) expected error, got nil", tt.input)
			}
		} else {
			if err != nil {
				t.Errorf("ParseFloat(%q) unexpected error: %v", tt.input, err)
			}
			if result != tt.expected {
				t.Errorf("ParseFloat(%q) = %f, expected %f", tt.input, result, tt.expected)
			}
		}
	}
}

func TestParseBool(t *testing.T) {
	tests := []struct {
		input    string
		expected bool
		hasError bool
	}{
		{"true", true, false},
		{"false", false, false},
		{"1", true, false},
		{"0", false, false},
		{"yes", true, false},
		{"no", false, false},
		{"是", true, false},
		{"否", false, false},
		{"", false, false},
		{"TRUE", true, false},
		{"FALSE", false, false},
		{"invalid", false, true},
	}

	for _, tt := range tests {
		result, err := ParseBool(tt.input)
		if tt.hasError {
			if err == nil {
				t.Errorf("ParseBool(%q) expected error, got nil", tt.input)
			}
		} else {
			if err != nil {
				t.Errorf("ParseBool(%q) unexpected error: %v", tt.input, err)
			}
			if result != tt.expected {
				t.Errorf("ParseBool(%q) = %v, expected %v", tt.input, result, tt.expected)
			}
		}
	}
}

func TestParseIntArray(t *testing.T) {
	tests := []struct {
		input     string
		separator string
		expected  []int64
		hasError  bool
	}{
		{"1,2,3", ",", []int64{1, 2, 3}, false},
		{"1|2|3", "|", []int64{1, 2, 3}, false},
		{"", ",", nil, false},
		{"1, 2, 3", ",", []int64{1, 2, 3}, false},
		{"1,2,a", ",", nil, true},
	}

	for _, tt := range tests {
		result, err := ParseIntArray(tt.input, tt.separator)
		if tt.hasError {
			if err == nil {
				t.Errorf("ParseIntArray(%q, %q) expected error, got nil", tt.input, tt.separator)
			}
		} else {
			if err != nil {
				t.Errorf("ParseIntArray(%q, %q) unexpected error: %v", tt.input, tt.separator, err)
			}
			if len(result) != len(tt.expected) {
				t.Errorf("ParseIntArray(%q, %q) = %v, expected %v", tt.input, tt.separator, result, tt.expected)
			} else {
				for i, v := range result {
					if v != tt.expected[i] {
						t.Errorf("ParseIntArray(%q, %q)[%d] = %d, expected %d", tt.input, tt.separator, i, v, tt.expected[i])
					}
				}
			}
		}
	}
}

func TestParseStringArray(t *testing.T) {
	tests := []struct {
		input     string
		separator string
		expected  []string
	}{
		{"a,b,c", ",", []string{"a", "b", "c"}},
		{"a|b|c", "|", []string{"a", "b", "c"}},
		{"", ",", nil},
		{"a, b, c", ",", []string{"a", "b", "c"}},
		{"hello", ",", []string{"hello"}},
	}

	for _, tt := range tests {
		result := ParseStringArray(tt.input, tt.separator)
		if len(result) != len(tt.expected) {
			t.Errorf("ParseStringArray(%q, %q) = %v, expected %v", tt.input, tt.separator, result, tt.expected)
		} else {
			for i, v := range result {
				if v != tt.expected[i] {
					t.Errorf("ParseStringArray(%q, %q)[%d] = %q, expected %q", tt.input, tt.separator, i, v, tt.expected[i])
				}
			}
		}
	}
}

func TestParseNestedField(t *testing.T) {
	tests := []struct {
		input     string
		separator string
		expected  map[string]string
		hasError  bool
	}{
		{"name=value", ";", map[string]string{"name": "value"}, false},
		{"a=1;b=2;c=3", ";", map[string]string{"a": "1", "b": "2", "c": "3"}, false},
		{"", ";", nil, false},
		{"invalid", ";", nil, true},
	}

	for _, tt := range tests {
		result, err := ParseNestedField(tt.input, tt.separator)
		if tt.hasError {
			if err == nil {
				t.Errorf("ParseNestedField(%q, %q) expected error, got nil", tt.input, tt.separator)
			}
		} else {
			if err != nil {
				t.Errorf("ParseNestedField(%q, %q) unexpected error: %v", tt.input, tt.separator, err)
			}
			if len(result) != len(tt.expected) {
				t.Errorf("ParseNestedField(%q, %q) = %v, expected %v", tt.input, tt.separator, result, tt.expected)
			} else {
				for k, v := range tt.expected {
					if result[k] != v {
						t.Errorf("ParseNestedField(%q, %q)[%q] = %q, expected %q", tt.input, tt.separator, k, result[k], v)
					}
				}
			}
		}
	}
}
