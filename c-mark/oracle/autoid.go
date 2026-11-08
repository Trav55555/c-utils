// Heading ids as Hugo v0.120.4 generates them for autoHeadingIDType
// "github" (markup/goldmark/autoid.go). Copied with the blackfriday and
// ASCII-only branches removed, which that setting never reaches.
// Copyright 2019 The Hugo Authors. Licensed under the Apache License,
// Version 2.0: http://www.apache.org/licenses/LICENSE-2.0
package main

import (
	"bytes"
	"strconv"
	"unicode"
	"unicode/utf8"

	"github.com/yuin/goldmark/ast"
	"github.com/yuin/goldmark/parser"
)

func sanitizeAnchorNameWithHook(b []byte, hook func(buf *bytes.Buffer)) []byte {
	var buf bytes.Buffer
	b = bytes.TrimSpace(b)
	for len(b) > 0 {
		r, size := utf8.DecodeRune(b)
		switch {
		case r == '-' || r == ' ':
			buf.WriteRune('-')
		case isAlphaNumeric(r):
			buf.WriteRune(unicode.ToLower(r))
		default:
		}
		b = b[size:]
	}
	if hook != nil {
		hook(&buf)
	}
	result := make([]byte, buf.Len())
	copy(result, buf.Bytes())
	return result
}

func isAlphaNumeric(r rune) bool {
	return r == '_' || unicode.IsLetter(r) || unicode.IsDigit(r)
}

var _ parser.IDs = (*idFactory)(nil)

type idFactory struct {
	vals map[string]struct{}
}

func newIDFactory() *idFactory {
	return &idFactory{vals: make(map[string]struct{})}
}

func (ids *idFactory) Generate(value []byte, kind ast.NodeKind) []byte {
	return sanitizeAnchorNameWithHook(value, func(buf *bytes.Buffer) {
		if buf.Len() == 0 {
			if kind == ast.KindHeading {
				buf.WriteString("heading")
			} else {
				buf.WriteString("id")
			}
		}
		if _, found := ids.vals[buf.String()]; found {
			buf.WriteRune('-')
			pos := buf.Len()
			for i := 1; ; i++ {
				buf.WriteString(strconv.Itoa(i))
				if _, found := ids.vals[buf.String()]; !found {
					break
				}
				buf.Truncate(pos)
			}
		}
		ids.vals[buf.String()] = struct{}{}
	})
}

func (ids *idFactory) Put(value []byte) {
	ids.vals[string(value)] = struct{}{}
}
