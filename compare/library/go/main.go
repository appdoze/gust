// compare/library's Go twin (library.goose says what this is).
package main

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"regexp"
	"slices"
	"strconv"
	"time"
)

type Rnd struct{ state uint32 }

func (r *Rnd) next() uint32 {
	r.state = r.state*1664525 + 1013904223
	return r.state
}
func (r *Rnd) value() float64 { return 0.001 + float64(r.next())*(1.0/4294967296.0)*1000.0 }

const NF = 400000
const NI = 2000000

func fmtf() float64 {
	rnd := Rnd{12345}
	out := make([]byte, 0, 64)
	sum := 0
	for i := 0; i < NF; i++ {
		out = strconv.AppendFloat(out[:0], rnd.value(), 'f', -1, 64)
		sum += len(out)
	}
	return float64(sum)
}

func parsef() float64 {
	rnd := Rnd{12345}
	var text []byte
	for i := 0; i < NF; i++ {
		if i > 0 {
			text = append(text, ' ')
		}
		text = strconv.AppendFloat(text, rnd.value(), 'f', -1, 64)
	}
	sum := 0.0
	for _, w := range bytes.Split(text, []byte(" ")) {
		if v, err := strconv.ParseFloat(string(w), 64); err == nil {
			sum += v
		}
	}
	return sum
}

func fmti() float64 {
	rnd := Rnd{777}
	out := make([]byte, 0, 64)
	sum := 0
	for i := 0; i < NI; i++ {
		out = strconv.AppendUint(out[:0], uint64(rnd.next()), 10)
		sum += len(out)
	}
	return float64(sum)
}

func parsei() float64 {
	rnd := Rnd{777}
	var text []byte
	for i := 0; i < NI; i++ {
		if i > 0 {
			text = append(text, ' ')
		}
		text = strconv.AppendUint(text, uint64(rnd.next()), 10)
	}
	sum := int64(0)
	for _, w := range bytes.Split(text, []byte(" ")) {
		if v, err := strconv.ParseInt(string(w), 10, 64); err == nil {
			sum += v
		}
	}
	return float64(sum)
}

const NW = 2000000

func makeText() []byte {
	rnd := Rnd{2026}
	var out []byte
	for i := 0; i < NW; i++ {
		if i > 0 {
			out = append(out, ' ')
		}
		out = append(out, 'w')
		out = strconv.AppendUint(out, uint64(rnd.next()%50000), 10)
	}
	return out
}

func findAll() float64 {
	text := makeText()
	total := 0
	needle := []byte("w49999 w")
	for r := 0; r < 3; r++ {
		sub := text
		for {
			p := bytes.Index(sub, needle)
			if p < 0 {
				break
			}
			total++
			sub = sub[p+1:]
		}
	}
	return float64(total)
}

func replaceAll() float64 {
	text := makeText()
	sum := 0
	for r := 0; r < 3; r++ {
		out := bytes.ReplaceAll(text, []byte("w1"), []byte("wordone"))
		sum += len(out)
	}
	return float64(sum)
}

func splitJoin() float64 {
	text := makeText()
	sum := 0
	for r := 0; r < 3; r++ {
		var out []byte
		rest := text
		for {
			p := bytes.IndexByte(rest, ' ')
			w := rest
			if p >= 0 {
				w = rest[:p]
			}
			if len(out) > 0 {
				out = append(out, ',')
			}
			out = append(out, w...)
			if p < 0 {
				break
			}
			rest = rest[p+1:]
		}
		sum += len(out)
	}
	return float64(sum)
}

func lower() float64 {
	text := makeText()
	sum := 0
	for r := 0; r < 20; r++ {
		text = bytes.ToUpper(text)
		text = bytes.ToLower(text)
		sum += int(text[r])
	}
	return float64(sum)
}

func sortStrings() float64 {
	text := makeText()
	words := bytes.Split(text[:len(text)/4], []byte(" "))
	strs := make([]string, len(words))
	for i, w := range words {
		strs[i] = string(w)
	}
	slices.Sort(strs)
	return float64(len(strs[0]) + len(strs[len(strs)-1]) + len(strs))
}

func sortInts() float64 {
	rnd := Rnd{7}
	var xs []int64
	for i := 0; i < 3000000; i++ {
		xs = append(xs, int64(rnd.next()))
	}
	slices.Sort(xs)
	return float64(xs[0] + xs[len(xs)/2])
}

func pushInts() float64 {
	sum := int64(0)
	for r := 0; r < 3; r++ {
		var xs []int64
		for i := int64(0); i < 20000000; i++ {
			xs = append(xs, i)
		}
		sum += xs[len(xs)-1]
	}
	return float64(sum)
}

func fileIO() float64 {
	text := makeText()
	sum := 0
	for r := 0; r < 5; r++ {
		os.WriteFile("gap_file.tmp", text, 0o644)
		back, _ := os.ReadFile("gap_file.tmp")
		sum += len(back)
	}
	os.Remove("gap_file.tmp")
	return float64(sum)
}

const NR = 200000

type Rec struct {
	ID    int     `json:"id"`
	Name  string  `json:"name"`
	Score float64 `json:"score"`
	OK    bool    `json:"ok"`
}

func records() []Rec {
	rnd := Rnd{31}
	out := make([]Rec, NR)
	for i := range out {
		name := "user" + strconv.FormatUint(uint64(rnd.next()%100000), 10)
		out[i] = Rec{i, name, float64(rnd.next()) * (1.0 / 4294967296.0) * 100.0, i%3 == 0}
	}
	return out
}

func jsonWrite() float64 {
	recs := records()
	sum := 0
	for r := 0; r < 3; r++ {
		b, _ := json.Marshal(recs)
		sum += len(b)
	}
	return float64(sum)
}

func jsonParse() float64 {
	text, _ := json.Marshal(records())
	sum := 0.0
	for r := 0; r < 3; r++ {
		var doc any
		json.Unmarshal(text, &doc)
		for _, it := range doc.([]any) {
			sum += it.(map[string]any)["score"].(float64)
		}
	}
	return sum
}

func b64() float64 {
	rnd := Rnd{5}
	data := make([]byte, 8000000)
	for i := range data {
		data[i] = byte(rnd.next())
	}
	sum := 0
	for r := 0; r < 3; r++ {
		enc := base64.StdEncoding.EncodeToString(data)
		dec, _ := base64.StdEncoding.DecodeString(enc)
		sum += len(enc) + len(dec)
	}
	return float64(sum)
}

func re() float64 {
	rnd := Rnd{2026}
	var text []byte
	for i := 0; i < 1000000; i++ {
		if i > 0 {
			text = append(text, ' ')
		}
		text = append(text, 'w')
		text = strconv.AppendUint(text, uint64(rnd.next()%50000), 10)
	}
	sum := 0
	for _, p := range []string{"w1[0-9]+ w2[0-9]+", "[a-z]+49[0-9]*", "(w12|w34|w56)[0-9] "} {
		sum += len(regexp.MustCompile(p).FindAllIndex(text, -1))
	}
	return float64(sum)
}

func main() {
	only := ""
	if len(os.Args) > 2 {
		only = os.Args[2]
	}
	for _, b := range []struct {
		name string
		f    func() float64
	}{{"fmtf", fmtf}, {"parsef", parsef}, {"fmti", fmti}, {"parsei", parsei},
		{"find", findAll}, {"replace", replaceAll}, {"splitjoin", splitJoin}, {"lower", lower},
		{"sortstr", sortStrings}, {"sortint", sortInts}, {"push", pushInts}, {"fileio", fileIO},
		{"jsonwrite", jsonWrite}, {"jsonparse", jsonParse}, {"base64", b64}, {"regex", re}} {
		if only != "" && only != b.name {
			continue
		}
		t0 := time.Now()
		ck := b.f()
		fmt.Printf("bench\t%s\t%v\t%v\n", b.name, time.Since(t0).Seconds(), strconv.FormatFloat(ck, 'f', -1, 64))
	}
}
