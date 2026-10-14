// Package prompt parses the Zenz prompt that Mozkey writes into the ZNZ1
// request body.
//
// The prompt is a flat UTF-8 string whose sections are introduced by private
// use area control characters; see src/session/zenz_prompt_builder.cc, which
// is the producer and therefore the authority on the layout.
package prompt

import "strings"

// Section markers, in the order the builder appends them.
const (
	ReadingBegin      rune = 0xEE00 // U+EE00 katakana reading
	OutputBegin       rune = 0xEE01 // U+EE01 start of the model output
	ContextBegin      rune = 0xEE02 // U+EE02 left context
	ProfileBegin      rune = 0xEE03 // U+EE03
	TopicBegin        rune = 0xEE04 // U+EE04
	StyleBegin        rune = 0xEE05 // U+EE05
	SettingsBegin     rune = 0xEE06 // U+EE06
	RightContextBegin rune = 0xEE07 // U+EE07 right context
)

// maxContextRunes caps the context text forwarded to Jev.  The wire protocol
// already caps the prompt at 8 KiB, but a huge left context mostly burns tokens
// without changing the decision, and the client only gives us ~3.5 s.
const maxContextRunes = 200

// maxConditionRunes caps the optional profile/topic/style/settings fields.
const maxConditionRunes = 64

// Parsed is the decoded prompt.  Reading holds the katakana reading exactly as
// received; use Hiragana() for dictionary and model input.
type Parsed struct {
	LeftContext  string
	RightContext string
	Profile      string
	Topic        string
	Style        string
	Settings     string
	Reading      string
}

// Parse splits a Zenz prompt into its sections.
//
// Control markers switch the active section and are never copied into the
// output.  Text before the first marker is ignored, matching the builder which
// always starts with ContextBegin.
func Parse(text string) Parsed {
	var result Parsed
	var section rune = -1
	var builder strings.Builder

	flush := func() {
		value := sanitize(builder.String())
		builder.Reset()
		switch section {
		case ContextBegin:
			result.LeftContext = clampRunes(value, maxContextRunes)
		case RightContextBegin:
			result.RightContext = clampRunes(value, maxContextRunes)
		case ReadingBegin:
			result.Reading = value
		case ProfileBegin:
			result.Profile = clampRunes(value, maxConditionRunes)
		case TopicBegin:
			result.Topic = clampRunes(value, maxConditionRunes)
		case StyleBegin:
			result.Style = clampRunes(value, maxConditionRunes)
		case SettingsBegin:
			result.Settings = clampRunes(value, maxConditionRunes)
		}
	}

	for _, r := range text {
		if isControl(r) {
			flush()
			section = r
			continue
		}
		builder.WriteRune(r)
	}
	flush()

	return result
}

// Hiragana converts the katakana reading to hiragana, which is the key space
// the Mozc dictionary is written in.
func (p Parsed) Hiragana() string {
	return KatakanaToHiragana(p.Reading)
}

func isControl(r rune) bool {
	return r >= 0xEE00 && r <= 0xEE0F
}

// sanitize mirrors ZenzPromptBuilder::SanitizeConditionText: drop Zenz control
// characters, fold line breaks and tabs to spaces, and drop the remaining C0
// controls.  The sections end up inside a JSON request body, so keeping the
// payload free of raw control bytes is not optional.
func sanitize(text string) string {
	var builder strings.Builder
	builder.Grow(len(text))

	for _, r := range text {
		switch {
		case isControl(r):
			continue
		case r == '\t' || r == '\n' || r == '\r':
			builder.WriteByte(' ')
		case r < 0x20 || r == 0x7F:
			continue
		default:
			builder.WriteRune(r)
		}
	}
	return builder.String()
}

// KatakanaToHiragana maps U+30A1..U+30F6 to U+3041..U+3096, matching
// ZenzPromptBuilder::HiraganaToKatakana run in reverse.
func KatakanaToHiragana(text string) string {
	var builder strings.Builder
	builder.Grow(len(text))

	for _, r := range text {
		if r >= 0x30A1 && r <= 0x30F6 {
			r -= 0x60
		}
		builder.WriteRune(r)
	}
	return builder.String()
}

func clampRunes(text string, limit int) string {
	if limit <= 0 {
		return ""
	}
	count := 0
	for i := range text {
		if count == limit {
			return text[:i]
		}
		count++
	}
	return text
}

// State renders the state string handed to Jev.
//
// 左文脈 / 読み / 右文脈 are the labels the bridge agreed on with the dataset
// format; the right context line is omitted when empty so the model never sees
// a dangling empty field.
func (p Parsed) State() string {
	var builder strings.Builder
	builder.WriteString("左文脈: ")
	builder.WriteString(p.LeftContext)
	builder.WriteString("\n読み: ")
	builder.WriteString(p.Hiragana())
	if p.RightContext != "" {
		builder.WriteString("\n右文脈: ")
		builder.WriteString(p.RightContext)
	}
	return builder.String()
}
