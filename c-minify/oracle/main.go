// Command oracle minifies stdin exactly as Hugo 0.120.4's --minify does for
// traversannan.com: tdewolff/minify v2.20.5 with Hugo's default tdewolff
// configuration (minifiers/config.go defaultTdewolffConfig) and the site's
// [minify.tdewolff.html] keepComments = true.
//
//	oracle -type text/html < page.html
//
// -type is a media type, matched as Hugo's minifiers.New registers them.
package main

import (
	"flag"
	"io"
	"os"
	"regexp"

	"github.com/tdewolff/minify/v2"
	"github.com/tdewolff/minify/v2/css"
	"github.com/tdewolff/minify/v2/html"
	"github.com/tdewolff/minify/v2/js"
	"github.com/tdewolff/minify/v2/json"
	"github.com/tdewolff/minify/v2/svg"
	"github.com/tdewolff/minify/v2/xml"
)

// New returns the minifier set Hugo builds (minifiers/minifiers.go New) for
// the site's configuration. keepComments is the site's override.
func New(keepComments bool) *minify.M {
	m := minify.New()
	m.Add("text/css", &css.Minifier{Precision: 0, KeepCSS2: true})
	jsm := &js.Minifier{Version: 2022}
	m.Add("text/javascript", jsm)
	m.AddRegexp(regexp.MustCompile("^(application|text)/(x-)?(java|ecma)script$"), jsm)
	jm := &json.Minifier{}
	m.Add("application/json", jm)
	m.AddRegexp(regexp.MustCompile(`^(application|text)/(x-|(ld|manifest)\+)?json$`), jm)
	m.Add("image/svg+xml", &svg.Minifier{KeepComments: false, Precision: 0})
	xm := &xml.Minifier{KeepWhitespace: false}
	m.Add("application/xml", xm)
	m.Add("application/rss+xml", xm)
	m.Add("text/xml", xm)
	m.Add("text/html", &html.Minifier{
		KeepComments:            keepComments,
		KeepDocumentTags:        true,
		KeepConditionalComments: true,
		KeepEndTags:             true,
		KeepDefaultAttrVals:     true,
		KeepWhitespace:          false,
	})
	return m
}

func main() {
	typ := flag.String("type", "text/html", "media type")
	keep := flag.Bool("keep-comments", true, "the site's html keepComments")
	flag.Parse()
	in, err := io.ReadAll(os.Stdin)
	check(err)
	out, err := New(*keep).Bytes(*typ, in)
	check(err)
	os.Stdout.Write(out)
}

func check(err error) {
	if err != nil {
		os.Stderr.WriteString("oracle: " + err.Error() + "\n")
		os.Exit(1)
	}
}
