package prompt

import "testing"

// buildPrompt mirrors ZenzPromptBuilder::Build so the tests use a prompt the
// real Mozkey client would produce.
func buildPrompt(left, right, profile, reading string) string {
	prompt := "\uee02" + left
	if right != "" {
		prompt += "\uee07" + right
	}
	if profile != "" {
		prompt += "\uee03" + profile
	}
	prompt += "\uee00" + reading + "\uee01"
	return prompt
}

func TestParseFullPrompt(t *testing.T) {
	got := Parse(buildPrompt("きょうは いい", "ですね", "ITエンジニア", "てんき"))

	if got.LeftContext != "きょうは いい" {
		t.Errorf("LeftContext = %q", got.LeftContext)
	}
	if got.RightContext != "ですね" {
		t.Errorf("RightContext = %q", got.RightContext)
	}
	if got.Profile != "ITエンジニア" {
		t.Errorf("Profile = %q", got.Profile)
	}
	if got.Reading != "てんき" {
		t.Errorf("Reading = %q, want the katakana-free form it already was", got.Reading)
	}
}

// The builder always appends OutputBegin, so the trailing marker must not leak
// into the reading or produce a trailing empty section.
func TestParseDropsTrailingOutputMarker(t *testing.T) {
	got := Parse("\uee02左\uee00テンキ\uee01")

	if got.Reading != "テンキ" {
		t.Errorf("Reading = %q, want テンキ", got.Reading)
	}
	if got.LeftContext != "左" {
		t.Errorf("LeftContext = %q, want 左", got.LeftContext)
	}
}

func TestParseWithoutRightContext(t *testing.T) {
	got := Parse("\uee02左文脈\uee00テンキ\uee01")

	if got.RightContext != "" {
		t.Errorf("RightContext = %q, want empty", got.RightContext)
	}
	if got.LeftContext != "左文脈" {
		t.Errorf("LeftContext = %q", got.LeftContext)
	}
}

func TestParseAllConditionSections(t *testing.T) {
	got := Parse("\uee02left\uee03profile\uee04topic\uee05style\uee06settings\uee00テンキ\uee01")

	want := map[string]string{
		"LeftContext": "left",
		"Profile":     "profile",
		"Topic":       "topic",
		"Style":       "style",
		"Settings":    "settings",
		"Reading":     "テンキ",
	}
	gotValues := map[string]string{
		"LeftContext": got.LeftContext,
		"Profile":     got.Profile,
		"Topic":       got.Topic,
		"Style":       got.Style,
		"Settings":    got.Settings,
		"Reading":     got.Reading,
	}
	for field, wantValue := range want {
		if gotValues[field] != wantValue {
			t.Errorf("%s = %q, want %q", field, gotValues[field], wantValue)
		}
	}
}

func TestParseStripsControlCharacters(t *testing.T) {
	got := Parse("\uee02きょう\nは\tいい\uee07右\uee00テンキ\uee01")

	// Line breaks and tabs are folded to spaces so the state string stays a
	// single logical line per field.
	if got.LeftContext != "きょう は いい" {
		t.Errorf("LeftContext = %q, want %q", got.LeftContext, "きょう は いい")
	}

	// A stray C0 control is dropped outright.
	got = Parse("\uee02a\x01b\uee00テンキ\uee01")
	if got.LeftContext != "ab" {
		t.Errorf("LeftContext = %q, want %q", got.LeftContext, "ab")
	}
}

func TestParseCapsContextLength(t *testing.T) {
	long := ""
	for i := 0; i < 500; i++ {
		long += "あ"
	}
	got := Parse("\uee02" + long + "\uee00テンキ\uee01")

	if len([]rune(got.LeftContext)) != maxContextRunes {
		t.Errorf("LeftContext length = %d runes, want %d",
			len([]rune(got.LeftContext)), maxContextRunes)
	}
}

func TestParseEmptyPrompt(t *testing.T) {
	got := Parse("")
	if got.Reading != "" || got.LeftContext != "" {
		t.Errorf("got %+v, want the zero value", got)
	}
}

func TestHiragana(t *testing.T) {
	got := Parse("\uee00テンキ\uee01").Hiragana()
	if got != "てんき" {
		t.Errorf("Hiragana = %q, want てんき", got)
	}
}

func TestKatakanaToHiragana(t *testing.T) {
	cases := map[string]string{
		"テンキ":  "てんき",
		"ニホン":  "にほん",
		"ｱｲｳ":  "ｱｲｳ", // halfwidth katakana is left alone
		"ひらがな": "ひらがな",
		"漢字混合": "漢字混合",
		"":     "",
	}
	for input, want := range cases {
		if got := KatakanaToHiragana(input); got != want {
			t.Errorf("KatakanaToHiragana(%q) = %q, want %q", input, got, want)
		}
	}
}

func TestState(t *testing.T) {
	got := Parse(buildPrompt("きょうは", "ですね", "", "てんき")).State()
	want := "左文脈: きょうは\n読み: てんき\n右文脈: ですね"
	if got != want {
		t.Errorf("State() = %q, want %q", got, want)
	}
}

// An empty right context must not leave a dangling field the model has to
// interpret.
func TestStateOmitsEmptyRightContext(t *testing.T) {
	got := Parse(buildPrompt("きょうは", "", "", "てんき")).State()
	want := "左文脈: きょうは\n読み: てんき"
	if got != want {
		t.Errorf("State() = %q, want %q", got, want)
	}
}
