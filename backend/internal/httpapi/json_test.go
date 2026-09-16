package httpapi

import (
	"strings"
	"testing"
)

func TestDecodeJSONStrict(t *testing.T) {
	type request struct {
		Name string `json:"name"`
	}

	tests := []struct {
		name    string
		body    string
		wantErr bool
	}{
		{name: "valid", body: `{"name":"station"}`},
		{name: "unknown field", body: `{"name":"station","ignored":true}`, wantErr: true},
		{name: "second value", body: `{"name":"station"} {"name":"other"}`, wantErr: true},
		{name: "trailing garbage", body: `{"name":"station"} trailing`, wantErr: true},
		{name: "empty", body: ``, wantErr: true},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			var decoded request
			err := DecodeJSONStrict(strings.NewReader(test.body), &decoded)
			if (err != nil) != test.wantErr {
				t.Fatalf("DecodeJSONStrict() error = %v, wantErr %v", err, test.wantErr)
			}
		})
	}
}
