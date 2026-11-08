package main

// The corpus: goldmark v1.6.0's own test cases, each with the options its
// _test.go file runs it with, and the oracle's exact output. Case files are
// read as goldmark's testutil.DoTestCaseFile reads them (testutil is not
// exported for this); that code is goldmark's, MIT, Copyright (c) 2019
// Yusuke Inuzuka.

import (
	"bufio"
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

type corpusCase struct {
	Suite       string  `json:"suite"`
	No          int     `json:"no"`
	Description string  `json:"description,omitempty"`
	Options     Options `json:"options"`
	Markdown    string  `json:"markdown"`
	HTML        string  `json:"html"`
}

type suite struct {
	name    string
	file    string // relative to the goldmark module root
	options Options
}

// The configurations of goldmark's own tests (commonmark_test.go,
// extra_test.go, options_test.go and extension/*_test.go). Tests that pass
// options c-mark does not port (custom linkify regexps, footnote id
// functions) are left out.
var suites = []suite{
	{"spec", "_test/spec.json", Options{XHTML: true, Unsafe: true}},
	{"extra", "_test/extra.txt", Options{XHTML: true, Unsafe: true}},
	{"options", "_test/options.txt", Options{Attribute: true, AutoHeadingID: true}},
	{"table", "extension/_test/table.txt", Options{Unsafe: true, XHTML: true, Table: true}},
	{"footnote", "extension/_test/footnote.txt", Options{Unsafe: true, Footnote: true}},
	{"strikethrough", "extension/_test/strikethrough.txt", Options{Unsafe: true, Strikethrough: true}},
	{"tasklist", "extension/_test/tasklist.txt", Options{Unsafe: true, TaskList: true}},
	{"definition_list", "extension/_test/definition_list.txt", Options{Unsafe: true, DefinitionList: true}},
	{"linkify", "extension/_test/linkify.txt", Options{Unsafe: true, Linkify: true}},
	{"typographer", "extension/_test/typographer.txt", Options{Unsafe: true, Typographer: true}},
}

type rawCase struct {
	no          int
	description string
	markdown    string
	expected    string
}

// writeCorpus renders every case twice: under its suite's options, checked
// against goldmark's recorded expectation as goldmark's tests check it
// (equal after trimming space), and under Hugo's configuration with raw
// HTML allowed, whose output only the oracle defines.
func writeCorpus(root string) error {
	var out []corpusCase
	disagree := 0
	for _, s := range suites {
		cases, err := readSuite(filepath.Join(root, s.file))
		if err != nil {
			return err
		}
		for _, c := range cases {
			got := string(render(s.options, []byte(c.markdown)))
			if strings.TrimSpace(got) != strings.TrimSpace(c.expected) {
				disagree++
				fmt.Fprintf(os.Stderr, "goldmark disagrees with its own case %s %d\n", s.name, c.no)
			}
			out = append(out, corpusCase{s.name, c.no, c.description, s.options, c.markdown, got})
			hugo := Hugo(true, false)
			out = append(out, corpusCase{"hugo/" + s.name, c.no, c.description, hugo, c.markdown,
				string(render(hugo, []byte(c.markdown)))})
		}
	}
	if disagree != 0 {
		return fmt.Errorf("%d cases disagree with goldmark's expectations", disagree)
	}
	enc := json.NewEncoder(os.Stdout)
	enc.SetEscapeHTML(false)
	enc.SetIndent("", " ")
	return enc.Encode(out)
}

func readSuite(path string) ([]rawCase, error) {
	if strings.HasSuffix(path, ".json") {
		data, err := os.ReadFile(path)
		if err != nil {
			return nil, err
		}
		var spec []struct {
			Markdown string `json:"markdown"`
			HTML     string `json:"html"`
			Example  int    `json:"example"`
			Section  string `json:"section"`
		}
		if err := json.Unmarshal(data, &spec); err != nil {
			return nil, err
		}
		cases := make([]rawCase, len(spec))
		for i, c := range spec {
			cases[i] = rawCase{c.Example, c.Section, c.Markdown, c.HTML}
		}
		return cases, nil
	}
	return readCaseFile(path)
}

const (
	attributeSeparator = "//- - - - - - - - -//"
	caseSeparator      = "//= = = = = = = = = = = = = = = = = = = = = = = =//"
)

var optionsRegexp = regexp.MustCompile(`(?i)\s*options:(.*)`)

// readCaseFile follows goldmark v1.6.0 testutil.DoTestCaseFile, with
// source() and expected() applied.
func readCaseFile(path string) ([]rawCase, error) {
	fp, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer fp.Close()
	scanner := bufio.NewScanner(fp)
	var cases []rawCase
	// goldmark keeps one case value for the whole file and never resets its
	// options, so an OPTIONS line applies to every later case until another
	// replaces it. Its tests depend on that (extra.txt 38 and 47).
	var opts struct{ EnableEscape, Trim bool }
	line := 0
	for scanner.Scan() {
		line++
		if strings.TrimSpace(scanner.Text()) == "" {
			continue
		}
		var c rawCase
		header := scanner.Text()
		if strings.Contains(header, ":") {
			parts := strings.Split(header, ":")
			c.no, err = strconv.Atoi(strings.TrimSpace(parts[0]))
			c.description = strings.Join(parts[1:], ":")
		} else {
			c.no, err = strconv.Atoi(header)
		}
		if err != nil {
			return nil, fmt.Errorf("%s: invalid case No at line %d", path, line)
		}
		if !scanner.Scan() {
			return nil, fmt.Errorf("%s: invalid case at line %d", path, line)
		}
		line++
		if m := optionsRegexp.FindAllStringSubmatch(scanner.Text(), -1); len(m) != 0 {
			if err := json.Unmarshal([]byte(m[0][1]), &opts); err != nil {
				return nil, fmt.Errorf("%s: invalid options at line %d", path, line)
			}
			scanner.Scan()
			line++
		}
		if scanner.Text() != attributeSeparator {
			return nil, fmt.Errorf("%s: invalid separator at line %d", path, line)
		}
		var buf []string
		for scanner.Scan() {
			line++
			if scanner.Text() == attributeSeparator {
				break
			}
			buf = append(buf, scanner.Text())
		}
		c.markdown = strings.Join(buf, "\n")
		buf = nil
		for scanner.Scan() {
			line++
			if scanner.Text() == caseSeparator {
				break
			}
			buf = append(buf, scanner.Text())
		}
		c.expected = strings.Join(buf, "\n")
		if len(c.expected) != 0 {
			c.expected += "\n"
		}
		if opts.Trim {
			c.markdown = strings.TrimSpace(c.markdown)
			c.expected = strings.TrimSpace(c.expected)
		}
		if opts.EnableEscape {
			c.markdown = string(applyEscapeSequence([]byte(c.markdown)))
			c.expected = string(applyEscapeSequence([]byte(c.expected)))
		}
		cases = append(cases, c)
	}
	return cases, scanner.Err()
}

func isHex(c byte) bool {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
}

// applyEscapeSequence is goldmark v1.6.0 testutil.applyEscapeSequence.
func applyEscapeSequence(b []byte) []byte {
	var result bytes.Buffer
	for i := 0; i < len(b); i++ {
		if b[i] == '\\' && i != len(b)-1 {
			switch b[i+1] {
			case 'a', 'b', 'f', 'n', 'r', 't', 'v', '\\':
				result.WriteByte(map[byte]byte{'a': '\a', 'b': '\b', 'f': '\f', 'n': '\n', 'r': '\r', 't': '\t', 'v': '\v', '\\': '\\'}[b[i+1]])
				i++
				continue
			case 'x':
				if len(b) >= i+3 && isHex(b[i+2]) && isHex(b[i+3]) {
					v, _ := hex.DecodeString(string(b[i+2 : i+4]))
					result.WriteByte(v[0])
					i += 3
					continue
				}
			case 'u', 'U':
				if len(b) > i+2 {
					var num []byte
					for j := i + 2; j < len(b); j++ {
						if isHex(b[j]) {
							num = append(num, b[j])
							continue
						}
						break
					}
					if len(num) >= 4 && len(num) < 8 {
						v, _ := strconv.ParseInt(string(num[:4]), 16, 32)
						result.WriteString(string(rune(v)))
						i += 5
						continue
					}
					if len(num) >= 8 {
						v, _ := strconv.ParseInt(string(num[:8]), 16, 32)
						result.WriteString(string(rune(v)))
						i += 9
						continue
					}
				}
			}
		}
		result.WriteByte(b[i])
	}
	return result.Bytes()
}
