// Package candidates resolves a kana reading to the Japanese surfaces the
// Mozc dictionary considers likely.
//
// The index is generated from src/data/dictionary_oss/dictionary*.txt by
// internal/candidates/gen and is checked in as a gzip TSV so the bridge starts
// fast and does not depend on the dictionary tree at runtime.
//
// File format, one reading per line, tab separated, ordered best first:
//
//	てんき <TAB> 天気 <TAB> 天鬼 <TAB> ...
package candidates

import (
	"bufio"
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"strings"
)

// Index maps a hiragana reading to its candidate surfaces, best first.
type Index struct {
	entries map[string][]string
}

// Load reads a gzip TSV index produced by internal/candidates/gen.
func Load(path string) (*Index, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("candidates: open %s: %w", path, err)
	}
	defer file.Close()

	gzipReader, err := gzip.NewReader(file)
	if err != nil {
		return nil, fmt.Errorf("candidates: gunzip %s: %w", path, err)
	}
	defer gzipReader.Close()

	index := &Index{entries: make(map[string][]string)}
	reader := bufio.NewReaderSize(gzipReader, 1<<20)

	for lineNumber := 1; ; lineNumber++ {
		line, readErr := reader.ReadString('\n')
		if line != "" {
			line = strings.TrimSuffix(line, "\n")
			line = strings.TrimSuffix(line, "\r")
			if line != "" {
				fields := strings.Split(line, "\t")
				if len(fields) < 2 {
					return nil, fmt.Errorf("candidates: %s:%d: expected reading and at least one surface", path, lineNumber)
				}
				// ReadString copies, so the slice keeps this line's storage and
				// is safe to hold after the buffer is reused.
				index.entries[fields[0]] = fields[1:]
			}
		}
		if readErr == io.EOF {
			break
		}
		if readErr != nil {
			return nil, fmt.Errorf("candidates: read %s: %w", path, readErr)
		}
	}

	return index, nil
}

// Len reports how many readings the index holds.
func (i *Index) Len() int {
	if i == nil {
		return 0
	}
	return len(i.entries)
}

// Surfaces returns the dictionary surfaces for reading, best first.
//
// It returns nil when the reading is absent, which the bridge treats as "no
// candidates" rather than as an empty candidate set: the two cases need
// different handling.
func (i *Index) Surfaces(reading string) []string {
	if i == nil || reading == "" {
		return nil
	}
	return i.entries[reading]
}
