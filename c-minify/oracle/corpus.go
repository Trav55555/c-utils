package main

// The corpus: tdewolff/minify v2.20.5's own HTML and XML test tables, each
// case under the options and minifier set its Go test runs it with, the
// same inputs under the site's configuration, the parse lexers' test
// inputs, and the live site's pages. Every expected output is the
// oracle's; the Go tests' own expectations are checked on the way.

import (
	"encoding/json"
	"errors"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"unicode/utf8"

	"github.com/tdewolff/minify/v2"
	"github.com/tdewolff/minify/v2/css"
	"github.com/tdewolff/minify/v2/html"
	"github.com/tdewolff/minify/v2/js"
	mjson "github.com/tdewolff/minify/v2/json"
	"github.com/tdewolff/minify/v2/svg"
	"github.com/tdewolff/minify/v2/xml"
)

// Options are the html.Minifier and xml.Minifier fields the corpus varies.
// KeepWhitespace is both minifiers' field.
type Options struct {
	KeepComments            bool `json:"keepComments,omitempty"`
	KeepConditionalComments bool `json:"keepConditionalComments,omitempty"`
	KeepDefaultAttrVals     bool `json:"keepDefaultAttrVals,omitempty"`
	KeepDocumentTags        bool `json:"keepDocumentTags,omitempty"`
	KeepEndTags             bool `json:"keepEndTags,omitempty"`
	KeepQuotes              bool `json:"keepQuotes,omitempty"`
	KeepWhitespace          bool `json:"keepWhitespace,omitempty"`
}

// Site is Hugo 0.120.4's default tdewolff configuration plus the site's
// keepComments.
func Site() Options {
	return Options{KeepComments: true, KeepConditionalComments: true, KeepDefaultAttrVals: true,
		KeepDocumentTags: true, KeepEndTags: true}
}

// slots name the embedded minifiers the HTML minifier can hand content to.
const (
	slotCSS  = "css"
	slotJS   = "js"
	slotJSON = "json"
	slotSVG  = "svg"
	slotXML  = "xml"
	slotHTML = "html"
)

var jsPattern = regexp.MustCompile("^(application|text)/(x-)?(java|ecma)script$")
var jsonPattern = regexp.MustCompile(`^(application|text)/(x-|(ld|manifest)\+)?json$`)

// recorder notes which slot was asked for and delegates to the minifier
// the configuration registers there, or reports that none exists exactly
// as an unregistered mimetype would.
type recorder struct {
	slot    string
	reached map[string]bool
	inner   minify.Minifier
}

func (r *recorder) Minify(m *minify.M, w io.Writer, rd io.Reader, params map[string]string) error {
	r.reached[r.slot] = true
	if r.inner == nil {
		return minify.ErrNotExist
	}
	return r.inner.Minify(m, w, rd, params)
}

// identity is the stand-in TestHTML registers for CSS and JS.
type identity struct{}

func (identity) Minify(_ *minify.M, w io.Writer, r io.Reader, _ map[string]string) error {
	_, err := io.Copy(w, r)
	return err
}

// embedded names what a configuration registers in each slot.
type embedded struct {
	css, js, json, svg, xml, html minify.Minifier
}

// hugoEmbedded is the set New builds, with o as the HTML options.
func hugoEmbedded(o Options) embedded {
	return embedded{
		css:  &css.Minifier{Precision: 0, KeepCSS2: true},
		js:   &js.Minifier{Version: 2022},
		json: &mjson.Minifier{},
		svg:  &svg.Minifier{KeepComments: false, Precision: 0},
		xml:  &xml.Minifier{KeepWhitespace: o.KeepWhitespace},
		html: htmlMinifier(o),
	}
}

func htmlMinifier(o Options) *html.Minifier {
	return &html.Minifier{
		KeepComments:            o.KeepComments,
		KeepConditionalComments: o.KeepConditionalComments,
		KeepDefaultAttrVals:     o.KeepDefaultAttrVals,
		KeepDocumentTags:        o.KeepDocumentTags,
		KeepEndTags:             o.KeepEndTags,
		KeepQuotes:              o.KeepQuotes,
		KeepWhitespace:          o.KeepWhitespace,
	}
}

// newM registers e under the mimetypes Hugo's minifiers.New uses, each
// behind a recorder, so an unregistered slot still records that it was
// asked for before failing with ErrNotExist.
func newM(e embedded, reached map[string]bool) *minify.M {
	m := minify.New()
	rec := func(slot string, inner minify.Minifier) *recorder { return &recorder{slot, reached, inner} }
	m.Add("text/css", rec(slotCSS, e.css))
	jsm := rec(slotJS, e.js)
	m.Add("text/javascript", jsm)
	m.Add("application/javascript", jsm)
	m.AddRegexp(jsPattern, jsm)
	jm := rec(slotJSON, e.json)
	m.Add("application/json", jm)
	m.AddRegexp(jsonPattern, jm)
	m.Add("image/svg+xml", rec(slotSVG, e.svg))
	xm := rec(slotXML, e.xml)
	m.Add("application/xml", xm)
	m.Add("application/rss+xml", xm)
	m.Add("text/xml", xm)
	m.Add("text/html", rec(slotHTML, e.html))
	return m
}

// result is one minification: the bytes, the slots reached, and the error
// the minifier returned, if any.
type result struct {
	out     []byte
	reached []string
	err     error
}

// run minifies src as mediatype typ with the top-level minifier the slot
// for typ holds (HTML or XML) and the embedded set e.
func run(typ string, o Options, e embedded, src []byte) result {
	reached := map[string]bool{}
	m := newM(e, reached)
	var top minify.Minifier
	switch typ {
	case "text/html":
		top = htmlMinifier(o)
	case "application/xml":
		top = &xml.Minifier{KeepWhitespace: o.KeepWhitespace}
	default:
		check(fmt.Errorf("corpus: no top-level minifier for %s", typ))
	}
	w := &strings.Builder{}
	err := top.Minify(m, w, strings.NewReader(string(src)), nil)
	names := make([]string, 0, len(reached))
	for s := range reached {
		names = append(names, s)
	}
	sort.Strings(names)
	return result{[]byte(w.String()), names, err}
}

// site minifies under the site's configuration with Hugo's minifier set.
func site(typ string, src []byte) result {
	return run(typ, Site(), hugoEmbedded(Site()), src)
}

type corpusCase struct {
	Suite       string   `json:"suite"`
	No          int      `json:"no"`
	Description string   `json:"description,omitempty"`
	Type        string   `json:"type"`
	Options     Options  `json:"options"`
	Input       string   `json:"input"`
	Expected    string   `json:"expected"`
	Reaches     []string `json:"reaches"`
	Error       string   `json:"error,omitempty"`
}

// suite is one Go test function's table: its file, options and embedded
// minifiers, as the test builds them.
type suite struct {
	name     string
	file     string // relative to the minify module root
	fn       string // the Test function holding the table
	typ      string
	options  Options
	embedded embedded
}

var testHTMLEmbedded = embedded{html: &html.Minifier{}, css: identity{}, js: identityAppJS{}}

// identityAppJS is TestHTML's identity minifier, registered only under
// application/javascript: text/javascript and the pattern stay unknown.
type identityAppJS struct{}

func (identityAppJS) Minify(m *minify.M, w io.Writer, r io.Reader, p map[string]string) error {
	return identity{}.Minify(m, w, r, p)
}

var suites = []suite{
	{"html", "html/html_test.go", "TestHTML", "text/html", Options{}, testHTMLEmbedded},
	{"html_cssjs", "html/html_test.go", "TestHTMLCSSJS", "text/html", Options{},
		embedded{html: &html.Minifier{}, css: &css.Minifier{}, js: &js.Minifier{}}},
	{"html_keep_end_tags", "html/html_test.go", "TestHTMLKeepEndTags", "text/html", Options{KeepEndTags: true}, embedded{}},
	{"html_keep_conditional_comments", "html/html_test.go", "TestHTMLKeepConditionalComments", "text/html",
		Options{KeepConditionalComments: true}, embedded{}},
	{"html_keep_whitespace", "html/html_test.go", "TestHTMLKeepWhitespace", "text/html", Options{KeepWhitespace: true}, embedded{}},
	{"html_keep_quotes", "html/html_test.go", "TestHTMLKeepQuotes", "text/html", Options{KeepQuotes: true}, embedded{}},
	{"xml", "xml/xml_test.go", "TestXML", "application/xml", Options{}, embedded{}},
	{"xml_keep_whitespace", "xml/xml_test.go", "TestXMLKeepWhitespace", "application/xml", Options{KeepWhitespace: true}, embedded{}},
}

// lexerSuites are parse's lexer tests: inputs only, rendered under the
// site's configuration.
var lexerSuites = []struct {
	name string
	file string // relative to the parse module root
	typ  string
	fns  []string
}{
	{"lex_html", "html/lex_test.go", "text/html", []string{"TestTokens", "TestTags", "TestAttributes", "TestErrors"}},
	{"lex_xml", "xml/lex_test.go", "application/xml", []string{"TestTokens", "TestTags", "TestAttributes", "TestErrors"}},
}

// testTable returns the string pairs of the first []struct{...}{...}
// literal assigned in fn: each element's first two string fields (input,
// expected). Elements with one string field give an empty expected.
func testTable(path, fn string) ([][2]string, error) {
	fset := token.NewFileSet()
	f, err := parser.ParseFile(fset, path, nil, 0)
	if err != nil {
		return nil, err
	}
	var table *ast.CompositeLit
	for _, d := range f.Decls {
		fd, ok := d.(*ast.FuncDecl)
		if !ok || fd.Name.Name != fn {
			continue
		}
		ast.Inspect(fd.Body, func(n ast.Node) bool {
			if table != nil {
				return false
			}
			// `tests := []struct{...}{...}` or `var tests = []struct{...}{...}`.
			var rhs ast.Expr
			switch s := n.(type) {
			case *ast.AssignStmt:
				if len(s.Rhs) == 1 {
					rhs = s.Rhs[0]
				}
			case *ast.ValueSpec:
				if len(s.Values) == 1 {
					rhs = s.Values[0]
				}
			}
			cl, ok := rhs.(*ast.CompositeLit)
			if !ok {
				return true
			}
			if _, ok := cl.Type.(*ast.ArrayType); ok {
				table = cl
				return false
			}
			return true
		})
	}
	if table == nil {
		return nil, fmt.Errorf("%s: no test table in %s", path, fn)
	}
	var out [][2]string
	for _, el := range table.Elts {
		row, ok := el.(*ast.CompositeLit)
		if !ok {
			return nil, fmt.Errorf("%s: %s: element is not a composite literal", path, fn)
		}
		var strs []string
		for _, e := range row.Elts {
			lit, ok := e.(*ast.BasicLit)
			if !ok || lit.Kind != token.STRING {
				continue
			}
			s, err := strconv.Unquote(lit.Value)
			if err != nil {
				return nil, fmt.Errorf("%s: %s: %v", path, fn, err)
			}
			strs = append(strs, s)
		}
		if len(strs) == 0 {
			return nil, fmt.Errorf("%s: %s: element without strings", path, fn)
		}
		var pair [2]string
		pair[0] = strs[0]
		if 1 < len(strs) {
			pair[1] = strs[1]
		}
		out = append(out, pair)
	}
	return out, nil
}

func (r result) toCase(suite string, no int, desc, typ string, o Options, input []byte) corpusCase {
	c := corpusCase{Suite: suite, No: no, Description: desc, Type: typ, Options: o, Input: string(input),
		Expected: string(r.out), Reaches: r.reached}
	if c.Reaches == nil {
		c.Reaches = []string{}
	}
	if r.err != nil {
		c.Error = r.err.Error()
		c.Expected = ""
	}
	if !utf8.ValidString(c.Input) || !utf8.ValidString(c.Expected) {
		check(fmt.Errorf("corpus: %s %d is not valid UTF-8; JSON cannot carry it", suite, no))
	}
	return c
}

// writeCorpus writes every case to stdout. minifyRoot and parseRoot are
// the module directories; liveDir holds a parity baseline (manifest.json
// and bodies/<sha256>); pagesDir holds the exporter's unminified pages
// (*/index.html), either may be empty.
func writeCorpus(minifyRoot, parseRoot, liveDir, pagesDir string) error {
	var out []corpusCase
	disagree := 0
	for _, s := range suites {
		rows, err := testTable(filepath.Join(minifyRoot, s.file), s.fn)
		if err != nil {
			return err
		}
		for i, row := range rows {
			input := []byte(row[0])
			r := run(s.typ, s.options, s.embedded, input)
			if r.err != nil || string(r.out) != row[1] {
				disagree++
				fmt.Fprintf(os.Stderr, "minify disagrees with its own case %s %d: %q -> %q (want %q, err %v)\n",
					s.name, i+1, row[0], r.out, row[1], r.err)
			}
			out = append(out, r.toCase(s.name, i+1, "", s.typ, s.options, input))
			out = append(out, site(s.typ, input).toCase("site/"+s.name, i+1, "", s.typ, Site(), input))
		}
	}
	if disagree != 0 {
		return fmt.Errorf("%d cases disagree with minify's expectations", disagree)
	}
	for _, s := range lexerSuites {
		no := 0
		for _, fn := range s.fns {
			rows, err := testTable(filepath.Join(parseRoot, s.file), fn)
			if err != nil {
				return err
			}
			for _, row := range rows {
				no++
				input := []byte(row[0])
				out = append(out, site(s.typ, input).toCase("site/"+s.name, no, fn, s.typ, Site(), input))
			}
		}
	}
	if liveDir != "" {
		live, err := liveCases(liveDir)
		if err != nil {
			return err
		}
		out = append(out, live...)
	}
	if pagesDir != "" {
		pages, err := filepath.Glob(filepath.Join(pagesDir, "*", "index.html"))
		if err != nil {
			return err
		}
		sort.Strings(pages)
		if len(pages) == 0 {
			return errors.New("corpus: no */index.html under " + pagesDir)
		}
		for i, p := range pages {
			src, err := os.ReadFile(p)
			if err != nil {
				return err
			}
			rel, _ := filepath.Rel(pagesDir, p)
			out = append(out, site("text/html", src).toCase("exporter", i+1, rel, "text/html", Site(), src))
		}
	}
	enc := json.NewEncoder(os.Stdout)
	enc.SetEscapeHTML(false)
	enc.SetIndent("", " ")
	return enc.Encode(out)
}

// liveCases turns the baseline's HTML and XML bodies (status 200, one case
// per distinct body, in URL order) into idempotence cases: the live bytes
// are already minified, and the expected output is what the oracle makes
// of them.
func liveCases(dir string) ([]corpusCase, error) {
	data, err := os.ReadFile(filepath.Join(dir, "manifest.json"))
	if err != nil {
		return nil, err
	}
	var manifest struct {
		Entries []struct {
			Status int    `json:"status"`
			Type   string `json:"type"`
			URL    string `json:"url"`
			SHA256 string `json:"sha256"`
		} `json:"entries"`
	}
	if err := json.Unmarshal(data, &manifest); err != nil {
		return nil, err
	}
	var out []corpusCase
	for _, typ := range []string{"text/html", "application/xml"} {
		seen := map[string]bool{}
		type body struct{ url, sha string }
		var bodies []body
		for _, e := range manifest.Entries {
			if e.Status != 200 || e.Type != typ || seen[e.SHA256] {
				continue
			}
			seen[e.SHA256] = true
			bodies = append(bodies, body{e.URL, e.SHA256})
		}
		sort.Slice(bodies, func(i, j int) bool { return bodies[i].url < bodies[j].url })
		name := "live_html"
		if typ != "text/html" {
			name = "live_xml"
		}
		for i, b := range bodies {
			src, err := os.ReadFile(filepath.Join(dir, "bodies", b.sha))
			if err != nil {
				return nil, err
			}
			out = append(out, site(typ, src).toCase(name, i+1, b.url, typ, Site(), src))
		}
	}
	return out, nil
}
