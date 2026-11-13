// Command oracle renders Markdown with goldmark v1.6.0. It is the reference
// c-mark is tested against and is never linked into it.
//
//	oracle -hugo [-unsafe] [-site] < in.md > out.html
//	oracle -cases cases.json > outputs.json
//	oracle -corpus "$(go list -m -f '{{.Dir}}' github.com/yuin/goldmark)" > corpus.json
//
// -hugo configures goldmark as Hugo 0.120.4 does by default
// (markup/goldmark/convert.go with goldmark_config.Default): tables,
// strikethrough, linkify, task lists, the typographer with HTML-entity
// substitutions, definition lists, footnotes, auto heading ids generated as
// Hugo's "github" id type does, and heading attributes. -unsafe passes raw
// HTML through (the site sets markup.goldmark.renderer.unsafe). -site renders
// fenced code as the site's render-codeblock.html hook does.
//
// -cases reads a JSON array of {"options": Options, "markdown": "..."} and
// writes a JSON array of the rendered strings, so a run costs one process.
// -corpus writes goldmark's own test cases with their options and the
// oracle's output (corpus.go).
package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"

	"github.com/yuin/goldmark"
	"github.com/yuin/goldmark/ast"
	"github.com/yuin/goldmark/extension"
	"github.com/yuin/goldmark/parser"
	"github.com/yuin/goldmark/renderer"
	"github.com/yuin/goldmark/renderer/html"
	"github.com/yuin/goldmark/text"
	"github.com/yuin/goldmark/util"
)

// Options names one goldmark configuration. The zero value is goldmark.New()
// with no extensions and no options.
type Options struct {
	Table          bool   `json:"table,omitempty"`
	TableAlign     string `json:"tableAlign,omitempty"` // "", "default", "attribute", "style", "none"
	Strikethrough  bool   `json:"strikethrough,omitempty"`
	Linkify        bool   `json:"linkify,omitempty"`
	TaskList       bool   `json:"taskList,omitempty"`
	Typographer    bool   `json:"typographer,omitempty"`
	DefinitionList bool   `json:"definitionList,omitempty"`
	Footnote       bool   `json:"footnote,omitempty"`
	AutoHeadingID  bool   `json:"autoHeadingID,omitempty"`
	Attribute      bool   `json:"attribute,omitempty"`
	HugoIDs        bool   `json:"hugoIDs,omitempty"` // Hugo's id factory instead of goldmark's
	Unsafe         bool   `json:"unsafe,omitempty"`
	XHTML          bool   `json:"xhtml,omitempty"`
	HardWraps      bool   `json:"hardWraps,omitempty"`
	SiteCode       bool   `json:"siteCode,omitempty"`
}

// Hugo returns Hugo 0.120.4's default configuration.
func Hugo(unsafe, site bool) Options {
	return Options{Table: true, Strikethrough: true, Linkify: true, TaskList: true, Typographer: true,
		DefinitionList: true, Footnote: true, AutoHeadingID: true, Attribute: true, HugoIDs: true,
		Unsafe: unsafe, SiteCode: site}
}

type testCase struct {
	Options  Options `json:"options"`
	Markdown string  `json:"markdown"`
}

func main() {
	hugo := flag.Bool("hugo", false, "Hugo 0.120.4's default configuration")
	unsafe := flag.Bool("unsafe", false, "render raw HTML")
	site := flag.Bool("site", false, "render fenced code as the site's hook does")
	cases := flag.String("cases", "", "JSON array of {options, markdown}")
	corpus := flag.String("corpus", "", "goldmark module root: write the test corpus to stdout")
	flag.Parse()

	if *corpus != "" {
		check(writeCorpus(*corpus))
		return
	}

	if *cases != "" {
		data, err := os.ReadFile(*cases)
		check(err)
		var in []testCase
		check(json.Unmarshal(data, &in))
		out := make([]string, len(in))
		for i, c := range in {
			out[i] = string(render(c.Options, []byte(c.Markdown)))
		}
		enc, err := json.Marshal(out)
		check(err)
		os.Stdout.Write(enc)
		return
	}
	src, err := io.ReadAll(os.Stdin)
	check(err)
	opts := Options{Unsafe: *unsafe, SiteCode: *site}
	if *hugo {
		opts = Hugo(*unsafe, *site)
	}
	os.Stdout.Write(render(opts, src))
}

func check(err error) {
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func render(o Options, src []byte) []byte {
	var buf bytes.Buffer
	ctx := parser.NewContext()
	if o.HugoIDs {
		ctx = parser.NewContext(parser.WithIDs(newIDFactory()))
	}
	check(newMarkdown(o).Convert(src, &buf, parser.WithContext(ctx)))
	return buf.Bytes()
}

func newMarkdown(o Options) goldmark.Markdown {
	var ropts []renderer.Option
	if o.Unsafe {
		ropts = append(ropts, html.WithUnsafe())
	}
	if o.XHTML {
		ropts = append(ropts, html.WithXHTML())
	}
	if o.HardWraps {
		ropts = append(ropts, html.WithHardWraps())
	}
	var exts []goldmark.Extender
	if o.Table {
		switch o.TableAlign {
		case "":
			exts = append(exts, extension.Table)
		case "default":
			exts = append(exts, extension.NewTable(extension.WithTableCellAlignMethod(extension.TableCellAlignDefault)))
		case "attribute":
			exts = append(exts, extension.NewTable(extension.WithTableCellAlignMethod(extension.TableCellAlignAttribute)))
		case "style":
			exts = append(exts, extension.NewTable(extension.WithTableCellAlignMethod(extension.TableCellAlignStyle)))
		case "none":
			exts = append(exts, extension.NewTable(extension.WithTableCellAlignMethod(extension.TableCellAlignNone)))
		default:
			check(fmt.Errorf("unknown tableAlign %q", o.TableAlign))
		}
	}
	if o.Strikethrough {
		exts = append(exts, extension.Strikethrough)
	}
	if o.Linkify {
		exts = append(exts, extension.Linkify)
	}
	if o.TaskList {
		exts = append(exts, extension.TaskList)
	}
	if o.Typographer {
		// Hugo passes its configured substitutions; its defaults are the
		// same HTML entities as goldmark's (newDefaultSubstitutions).
		exts = append(exts, extension.Typographer)
	}
	if o.DefinitionList {
		exts = append(exts, extension.DefinitionList)
	}
	if o.Footnote {
		exts = append(exts, extension.Footnote)
	}
	if o.SiteCode {
		exts = append(exts, siteCode{})
	}
	var popts []parser.Option
	if o.AutoHeadingID {
		popts = append(popts, parser.WithAutoHeadingID())
	}
	if o.Attribute {
		popts = append(popts, parser.WithAttribute())
	}
	return goldmark.New(goldmark.WithExtensions(exts...), goldmark.WithParserOptions(popts...),
		goldmark.WithRendererOptions(ropts...))
}

// siteCode renders fenced code blocks as travers-personal-site's
// layouts/_default/_markup/render-codeblock.html does under Hugo 0.120.4.
type siteCode struct{}

func (siteCode) Extend(m goldmark.Markdown) {
	m.Renderer().AddOptions(renderer.WithNodeRenderers(util.Prioritized(siteCodeRenderer{}, 100)))
}

type siteCodeRenderer struct{}

func (siteCodeRenderer) RegisterFuncs(reg renderer.NodeRendererFuncRegisterer) {
	reg.Register(ast.KindFencedCodeBlock, func(w util.BufWriter, src []byte, node ast.Node, entering bool) (ast.WalkStatus, error) {
		if !entering {
			return ast.WalkContinue, nil
		}
		n := node.(*ast.FencedCodeBlock)
		// The template reads .Attributes.lang, which Hugo fills only from a
		// {lang=...} attribute in the info string; the info-string language
		// is .Type, which the template never reads. So the live class is
		// "language-" unless an attribute names one.
		lang, err := siteLang(n, src)
		if err != nil {
			return ast.WalkStop, err
		}
		var inner bytes.Buffer
		lines := n.Lines()
		for i := 0; i < lines.Len(); i++ {
			seg := lines.At(i)
			inner.Write(seg.Value(src))
		}
		w.WriteString(`<pre><code class="language-`)
		w.WriteString(lang)
		w.WriteString(`">`)
		// Hugo's .Inner is the text with trailing newlines chomped.
		w.WriteString(htmlEscape(strings.TrimRight(inner.String(), "\r\n")))
		w.WriteString("</code></pre>\n")
		return ast.WalkSkipChildren, nil
	})
}

// siteLang is `{{ with .Attributes.lang }}{{ $lang = (lower .) }}{{ end }}`
// under Hugo 0.120.4. Hugo routes a block whose language is "mermaid" to
// render-codeblock-mermaid.html instead (codeblocks/render.go Render,
// getLang); that hook is not reproduced, so such a block is an error.
// .Attributes comes from codeblocks/render.go getAttributes: goldmark sets
// no attributes on fenced code, so Hugo parses the span from the last '{'
// before the first '}' of the info string with parser.ParseAttributes, and
// fails the build when that span does not parse. attributes.New lowercases
// names, drops on* names, keeps []byte values as strings and bool/float64
// as they are, and panics on any other value; Attributes() builds a map in
// order, so the last "lang" wins. `with` skips false, 0 and "", and lower
// casts with cast.ToString (floats as strconv 'f', -1).
func siteLang(n *ast.FencedCodeBlock, src []byte) (string, error) {
	language := string(n.Language(src))
	if l, _, _ := strings.Cut(language, "{"); l == "mermaid" {
		return "", fmt.Errorf("mermaid code blocks use a hook c-mark does not reproduce")
	}
	if n.Info == nil {
		return "", nil
	}
	info := n.Info.Segment.Value(src)
	start, end := -1, -1
	for i, c := range info {
		if end == -1 && c == '{' {
			start = i
		}
		if start != -1 && c == '}' {
			end = i
			break
		}
	}
	if start == -1 || end == -1 {
		return "", nil
	}
	attrs, ok := parser.ParseAttributes(text.NewReader(info[start : end+1]))
	if !ok {
		return "", fmt.Errorf("failed to parse Markdown attributes; you may need to quote the values")
	}
	holder := ast.NewTextBlock()
	for _, a := range attrs {
		holder.SetAttribute(a.Name, a.Value)
	}
	var lang any
	found := false
	for _, a := range holder.Attributes() {
		name := strings.ToLower(string(a.Name))
		if strings.HasPrefix(name, "on") {
			continue
		}
		var v any
		switch vv := a.Value.(type) {
		case bool, float64:
			v = vv
		case []byte:
			v = string(vv)
		case []any:
			v = vv // hl_lines ranges in Hugo; never a usable lang
		default:
			return "", fmt.Errorf("hugo panics: attribute value not implemented: %T", vv)
		}
		if name == "lang" {
			lang, found = v, true
		}
	}
	if !found {
		return "", nil
	}
	switch v := lang.(type) {
	case string:
		return strings.ToLower(v), nil
	case bool:
		if !v {
			return "", nil
		}
		return "true", nil
	case float64:
		if v == 0 {
			return "", nil
		}
		return strconv.FormatFloat(v, 'f', -1, 64), nil
	}
	return "", fmt.Errorf("a list-valued lang attribute is not reproduced")
}

// htmlEscape is Go's template htmlEscape as Hugo's htmlEscape function
// applies it: & < > " ' become &amp; &lt; &gt; &#34; &#39;.
func htmlEscape(s string) string {
	var b bytes.Buffer
	for _, r := range s {
		switch r {
		case '&':
			b.WriteString("&amp;")
		case '<':
			b.WriteString("&lt;")
		case '>':
			b.WriteString("&gt;")
		case '"':
			b.WriteString("&#34;")
		case '\'':
			b.WriteString("&#39;")
		default:
			b.WriteRune(r)
		}
	}
	return b.String()
}
