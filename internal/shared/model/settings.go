package model

type ServerSettings map[string]any

type SettingSchema struct {
	Key          string   `json:"key"`
	DisplayName  string   `json:"display_name"`
	Description  string   `json:"description"`
	Type         string   `json:"type"`
	DefaultValue any      `json:"default_value"`
	Group        string   `json:"group"`
	Min          *float64 `json:"min,omitempty"`
	Max          *float64 `json:"max,omitempty"`
	Options      []string `json:"options,omitempty"`
}
