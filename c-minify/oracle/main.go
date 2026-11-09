// Command oracle minifies exactly as Hugo 0.120.4's --minify does for
// traversannan.com: tdewolff/minify v2.20.5 with Hugo's default tdewolff
// configuration (minifiers/config.go defaultTdewolffConfig) and the site's
// [minify.tdewolff.html] keepComments = true. It is the reference c-minify
// is tested against and is never linked into it.
//
//	oracle [-type text/html] [-options JSON] < in > out
//	oracle -cases cases.json > results.json
//	oracle -corpus MINIFY_DIR -parse PARSE_DIR [-live DIR] [-pages DIR] > corpus.json
//
// -type is a media type, matched as Hugo's minifiers.New registers them;
// only text/html and application/xml are top-level types here. -options
// overrides the site's html/xml options (corpus.go Options). stdin mode
// reports the embedded minifiers the input reached on stderr as
// "reaches: css,svg" (corpus.go slots), so a differential test can skip
// inputs that route to a minifier c-minify does not port.
//
// -cases reads a JSON array of {"type", "options", "input"} with input in
// base64 (so it can carry any bytes) and writes a JSON array of {"output"
// (base64), "reaches", "error"}, one process for many documents.
// -corpus writes the test corpus (corpus.go).
package main

import (
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"

	"github.com/tdewolff/minify/v2"
)

// New returns the minifier set Hugo builds (minifiers/minifiers.go New) for
// the site's configuration: css {Precision 0, KeepCSS2}, js {Version 2022},
// json, svg {Precision 0}, xml {KeepWhitespace false} under application/xml,
// application/rss+xml and text/xml, and html {KeepDocumentTags,
// KeepConditionalComments, KeepEndTags, KeepDefaultAttrVals; KeepWhitespace
// false}. keepComments is the site's override. reached, if not nil, is
// filled with the slots the minification asks for.
func New(keepComments bool, reached map[string]bool) *minify.M {
	o := Site()
	o.KeepComments = keepComments
	if reached == nil {
		reached = map[string]bool{}
	}
	return newM(hugoEmbedded(o), reached)
}

type caseIn struct {
	Type    string  `json:"type"`
	Options Options `json:"options"`
	Input   string  `json:"input"` // base64
}

type caseOut struct {
	Output  string   `json:"output"` // base64
	Reaches []string `json:"reaches"`
	Error   string   `json:"error,omitempty"`
}

func main() {
	typ := flag.String("type", "text/html", "media type")
	optJSON := flag.String("options", "", "JSON html/xml options overriding the site's")
	cases := flag.String("cases", "", "JSON array of {type, options, input(base64)}")
	corpus := flag.String("corpus", "", "minify module root: write the test corpus to stdout")
	parseRoot := flag.String("parse", "", "parse module root (with -corpus)")
	live := flag.String("live", "", "parity baseline directory with manifest.json and bodies/ (with -corpus)")
	pages := flag.String("pages", "", "directory of unminified */index.html pages (with -corpus)")
	flag.Parse()

	if *corpus != "" {
		if *parseRoot == "" {
			check(fmt.Errorf("-corpus needs -parse"))
		}
		check(writeCorpus(*corpus, *parseRoot, *live, *pages))
		return
	}
	if *cases != "" {
		data, err := os.ReadFile(*cases)
		check(err)
		var in []caseIn
		check(json.Unmarshal(data, &in))
		out := make([]caseOut, len(in))
		for i, c := range in {
			src, err := base64.StdEncoding.DecodeString(c.Input)
			check(err)
			r := run(c.Type, c.Options, hugoEmbedded(c.Options), src)
			out[i] = caseOut{Output: base64.StdEncoding.EncodeToString(r.out), Reaches: r.reached}
			if r.reached == nil {
				out[i].Reaches = []string{}
			}
			if r.err != nil {
				out[i].Error = r.err.Error()
			}
		}
		enc, err := json.Marshal(out)
		check(err)
		os.Stdout.Write(enc)
		return
	}
	src, err := io.ReadAll(os.Stdin)
	check(err)
	var o Options
	if *optJSON == "" {
		o = Site()
	} else {
		check(json.Unmarshal([]byte(*optJSON), &o))
	}
	r := run(*typ, o, hugoEmbedded(o), src)
	fmt.Fprintf(os.Stderr, "reaches: %s\n", strings.Join(r.reached, ","))
	check(r.err)
	os.Stdout.Write(r.out)
}

func check(err error) {
	if err != nil {
		os.Stderr.WriteString("oracle: " + err.Error() + "\n")
		os.Exit(1)
	}
}
