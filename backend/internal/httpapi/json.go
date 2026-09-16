package httpapi

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
)

// DecodeJSONStrict decodes exactly one JSON value and rejects fields that are
// not declared by the destination type.
func DecodeJSONStrict(reader io.Reader, target any) error {
	decoder := json.NewDecoder(reader)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		return err
	}

	var trailing any
	if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
		if err == nil {
			return errors.New("request body must contain exactly one JSON value")
		}
		return err
	}
	return nil
}

// DecodeJSONBytesStrict is the byte-slice form used by handlers that retain
// the original body for an idempotency hash.
func DecodeJSONBytesStrict(body []byte, target any) error {
	return DecodeJSONStrict(bytes.NewReader(body), target)
}
