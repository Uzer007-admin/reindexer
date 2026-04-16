package reindexer

import (
	"math/rand"
	"strings"

	"github.com/restream/reindexer/v5"
	_ "github.com/restream/reindexer/v5/bindings/builtin"
	_ "github.com/restream/reindexer/v5/bindings/cproto"
)

const (
	charset  = "abcdefghijklmnopqrstuvwxyz" + "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
	nilUuid  = "00000000-0000-0000-0000-000000000000"
	hexChars = "0123456789abcdef"
)

var (
	adjectives = [...]string{"able", "above", "absolute", "balanced", "becoming", "beloved", "calm", "capable", "capital", "destined", "devoted", "direct", "enabled", "enabling", "endless", "factual", "fair", "faithful", "grand", "grateful", "great", "humane", "humble", "humorous", "ideal", "immense", "immortal", "joint", "just", "keen", "key", "kind", "logical", "loved", "loving", "mint", "model", "modern", "nice", "noble", "normal", "one", "open", "optimal", "polite", "popular", "positive", "quality", "quick", "quiet", "rapid", "rare", "rational", "sacred", "safe", "saved", "tight", "together", "tolerant", "unbiased", "uncommon", "unified", "valid", "valued", "vast", "wealthy", "welcome"}
	names      = [...]string{"ox", "ant", "ape", "asp", "bat", "bee", "boa", "bug", "cat", "cod", "cow", "cub", "doe", "dog", "eel", "eft", "elf", "elk", "emu", "ewe", "fly", "fox", "gar", "gnu", "hen", "hog", "imp", "jay", "kid", "kit", "koi", "lab", "man", "owl", "pig", "pug", "pup", "ram", "rat", "ray", "yak", "bass", "bear", "bird", "boar", "buck", "bull", "calf", "chow", "clam", "colt", "crab", "crow", "dane", "deer", "dodo", "dory", "dove", "drum", "duck", "fawn", "fish", "flea", "foal", "fowl", "frog", "gnat", "goat", "grub", "gull", "hare", "hawk", "ibex", "joey", "kite", "kiwi", "lamb", "lark", "lion", "loon", "lynx", "mako", "mink", "mite", "mole", "moth", "mule", "mutt", "newt", "orca", "oryx", "pika", "pony", "puma", "seal", "shad", "slug", "sole", "stag", "stud", "swan", "tahr", "teal", "tick", "toad", "tuna", "wasp", "wolf", "worm", "wren", "yeti"}
	cyrillic   = [...]string{"жук", "сон", "кофе", "кирилица", "етти", "мышь", "кот"}
	devices    = [...]string{"iphone", "android", "smarttv", "stb", "ottstb"}
	locations  = [...]string{"mos", "ct", "dv", "sth", "vlg", "sib", "ural"}
)

type testRandSource interface {
	Int() int
	Intn(n int) int
	Int63() int64
	Float32() float32
	Float64() float64
}

type defaultTestRandSource struct{}

func (defaultTestRandSource) Int() int         { return rand.Int() }
func (defaultTestRandSource) Intn(n int) int   { return rand.Intn(n) }
func (defaultTestRandSource) Int63() int64     { return rand.Int63() }
func (defaultTestRandSource) Float32() float32 { return rand.Float32() }
func (defaultTestRandSource) Float64() float64 { return rand.Float64() }

func randString() string {
	return randStringWithRand(defaultTestRandSource{})
}

func randStringWithRand(rng testRandSource) string {
	return adjectives[rng.Int()%len(adjectives)] + "_" + names[rng.Int()%len(names)]
}

func randLangString() string {
	return randLangStringWithRand(defaultTestRandSource{})
}

func randLangStringWithRand(rng testRandSource) string {
	return adjectives[rng.Int()%len(adjectives)] + "_" + cyrillic[rng.Int()%len(cyrillic)]
}

func randSearchString() string {
	return randSearchStringWithRand(defaultTestRandSource{})
}

func randSearchStringWithRand(rng testRandSource) string {
	if rng.Int()%2 == 0 {
		return adjectives[rng.Int()%len(adjectives)]
	}
	return names[rng.Int()%len(names)]
}

func trueRandWord(length int) string {
	return trueRandWordWithRand(defaultTestRandSource{}, length)
}

func trueRandWordWithRand(rng testRandSource, length int) string {
	b := make([]byte, length)
	for i := range b {
		b[i] = charset[rng.Intn(len(charset))]
	}
	return string(b)
}

func randStringArr(cnt int) []string {
	return randStringArrWithRand(defaultTestRandSource{}, cnt)
}

func randStringArrWithRand(rng testRandSource, cnt int) []string {
	if cnt < 1 {
		return nil
	}
	arr := make([]string, 0, cnt)
	for i := 0; i < cnt; i++ {
		arr = append(arr, randStringWithRand(rng))
	}
	return arr
}

func randVect(dimension int) []float32 {
	return randVectWithRand(defaultTestRandSource{}, dimension)
}

func randVectWithRand(rng testRandSource, dimension int) []float32 {
	result := make([]float32, dimension)
	for i := 0; i < dimension; i++ {
		result[i] = rng.Float32()
	}
	return result
}

func randDevice() string {
	return randDeviceWithRand(defaultTestRandSource{})
}

func randDeviceWithRand(rng testRandSource) string {
	return devices[rng.Int()%len(devices)]
}

func randPostalCode() int {
	return randPostalCodeWithRand(defaultTestRandSource{})
}

func randPostalCodeWithRand(rng testRandSource) int {
	return rng.Int() % 999999
}

func randLocation() string {
	return randLocationWithRand(defaultTestRandSource{})
}

func randLocationWithRand(rng testRandSource) string {
	return locations[rng.Int()%len(locations)]
}

func randFloat(min int, max int) float64 {
	return randFloatWithRand(defaultTestRandSource{}, min, max)
}

func randFloatWithRand(rng testRandSource, min int, max int) float64 {
	divider := (1 << rng.Intn(10))
	min *= divider
	max *= divider
	return float64(rng.Intn(max-min)+min) / float64(divider)
}

func randPoint() reindexer.Point {
	return randPointWithRand(defaultTestRandSource{})
}

func randPointWithRand(rng testRandSource) reindexer.Point {
	return reindexer.Point{randFloatWithRand(rng, -10, 10), randFloatWithRand(rng, -10, 10)}
}

func randIntArr(cnt int, start int, rng int) (arr []int) {
	return randIntArrWithRand(defaultTestRandSource{}, cnt, start, rng)
}

func randIntArrWithRand(rng testRandSource, cnt int, start int, span int) (arr []int) {
	if cnt == 0 {
		return nil
	}
	arr = make([]int, 0, cnt)
	for range cnt {
		arr = append(arr, start+rng.Int()%span)
	}
	return arr
}

func randInt32Arr(cnt int, start int, rng int) (arr []int32) {
	return randInt32ArrWithRand(defaultTestRandSource{}, cnt, start, rng)
}

func randInt32ArrWithRand(rng testRandSource, cnt int, start int, span int) (arr []int32) {
	if cnt == 0 {
		return nil
	}
	arr = make([]int32, 0, cnt)
	for range cnt {
		arr = append(arr, int32(start+rng.Int()%span))
	}
	return arr
}

func randUuid() string {
	return randUuidWithRand(defaultTestRandSource{})
}

func randUuidWithRand(rng testRandSource) string {
	if rng.Int()%1000 == 0 {
		return nilUuid
	}
	var b strings.Builder
	b.Grow(36)
	for i := 0; i < 36; i++ {
		switch i {
		case 8, 13, 18, 23:
			b.WriteByte('-')
		case 19:
			b.WriteByte(hexChars[8+(rng.Int()%(len(hexChars)-8))])
		default:
			b.WriteByte(hexChars[rng.Int()%len(hexChars)])
		}
	}
	return b.String()
}

func randUuidArray(cnt int) []string {
	return randUuidArrayWithRand(defaultTestRandSource{}, cnt)
}

func randUuidArrayWithRand(rng testRandSource, cnt int) []string {
	if cnt < 1 {
		return nil
	}
	arr := make([]string, 0, cnt)
	for i := 0; i < cnt; i++ {
		arr = append(arr, randUuidWithRand(rng))
	}
	return arr
}
