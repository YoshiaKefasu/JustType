// Command gen builds the reading -> candidate index consumed by
// mozkey-jev-bridge from the Mozc OSS dictionary.
//
// The Mozc dictionary is a TSV of:
//
//	reading <TAB> left_id <TAB> right_id <TAB> cost <TAB> surface
//
// Lower cost means a more likely surface, which is exactly the ranking an IME
// shows the user.  We keep the cheapest N distinct surfaces per reading so the
// bridge can offer Jev a realistic candidate set.
//
// Usage:
//
//	go run ./internal/candidates/gen -dict ../../../../src/data/dictionary_oss/dictionary0*.txt \
//	  -out data/candidates.tsv.gz -max 6
package main

import (
	"bufio"
	"compress/gzip"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

type candidate struct {
	surface string
	cost    int
}

func main() {
	dictGlobs := flag.String("dict", "", "comma separated glob(s) of Mozc dictionary .txt files")
	outPath := flag.String("out", "data/candidates.tsv.gz", "output .tsv.gz path")
	maxSurfaces := flag.Int("max", 6, "max distinct surfaces kept per reading")
	flag.Parse()

	if *dictGlobs == "" {
		fmt.Fprintln(os.Stderr, "gen: -dict is required")
		os.Exit(2)
	}
	if *maxSurfaces < 1 {
		fmt.Fprintln(os.Stderr, "gen: -max must be >= 1")
		os.Exit(2)
	}

	paths, err := expandGlobs(strings.Split(*dictGlobs, ","))
	if err != nil {
		fmt.Fprintf(os.Stderr, "gen: %v\n", err)
		os.Exit(1)
	}
	if len(paths) == 0 {
		fmt.Fprintln(os.Stderr, "gen: no dictionary files matched")
		os.Exit(1)
	}

	// reading -> distinct surfaces ordered by ascending cost.
	byReading := make(map[string][]candidate)
	seen := make(map[string]map[string]bool)

	for _, path := range paths {
		if err := ingest(path, *maxSurfaces, byReading, seen); err != nil {
			fmt.Fprintf(os.Stderr, "gen: %v\n", err)
			os.Exit(1)
		}
	}

	readings := make([]string, 0, len(byReading))
	for reading := range byReading {
		readings = append(readings, reading)
	}
	sort.Strings(readings)

	if err := writeIndex(*outPath, readings, byReading); err != nil {
		fmt.Fprintf(os.Stderr, "gen: %v\n", err)
		os.Exit(1)
	}

	surfaces := 0
	for _, candidates := range byReading {
		surfaces += len(candidates)
	}
	fmt.Printf("gen: %d readings, %d surfaces -> %s\n",
		len(readings), surfaces, *outPath)
}

// expandGlobs resolves comma separated glob patterns to concrete file paths.
func expandGlobs(globs []string) ([]string, error) {
	var paths []string
	for _, glob := range globs {
		glob = strings.TrimSpace(glob)
		if glob == "" {
			continue
		}
		matches, err := filepath.Glob(glob)
		if err != nil {
			return nil, fmt.Errorf("glob %q: %w", glob, err)
		}
		if len(matches) == 0 {
			return nil, fmt.Errorf("glob %q matched nothing", glob)
		}
		sort.Strings(matches)
		paths = append(paths, matches...)
	}
	return paths, nil
}

// ingest streams one dictionary file, retaining the cheapest maxSurfaces
// distinct surfaces per reading.  Only surfaces better than the current worst
// are retained, so the per-reading slice never exceeds maxSurfaces + 1.
func ingest(path string, maxSurfaces int, byReading map[string][]candidate,
	seen map[string]map[string]bool) error {
	file, err := os.Open(path)
	if err != nil {
		return err
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024)

	line := 0
	for scanner.Scan() {
		line++
		text := scanner.Text()
		if text == "" {
			continue
		}

		fields := strings.SplitN(text, "\t", 5)
		if len(fields) != 5 {
			// The dictionary ships a few malformed rows; skipping them keeps
			// the index build deterministic instead of failing the whole run.
			continue
		}
		reading, surface := fields[0], fields[4]
		if reading == "" || surface == "" {
			continue
		}
		cost, err := parseIntField(fields[3])
		if err != nil {
			continue
		}

		readingSeen := seen[reading]
		if readingSeen == nil {
			readingSeen = make(map[string]bool)
			seen[reading] = readingSeen
		}
		if readingSeen[surface] {
			continue
		}

		candidates := byReading[reading]
		if len(candidates) < maxSurfaces {
			readingSeen[surface] = true
			candidates = append(candidates, candidate{surface: surface, cost: cost})
			byReading[reading] = candidates
			continue
		}

		// Full: only replace when this surface beats the current worst.
		worst := 0
		for i := 1; i < len(candidates); i++ {
			if candidates[i].cost > candidates[worst].cost {
				worst = i
			}
		}
		if cost >= candidates[worst].cost {
			continue
		}
		readingSeen[candidates[worst].surface] = false
		readingSeen[surface] = true
		candidates[worst] = candidate{surface: surface, cost: cost}
		byReading[reading] = candidates
	}
	return scanner.Err()
}

// parseIntField parses a non-negative dictionary cost.
//
// A leading sign is accepted so a dictionary revision that ships negative costs
// does not silently drop every row it touches: the ranking is by cost, and a
// skipped row changes which surfaces the index keeps.
func parseIntField(text string) (int, error) {
	if text == "" {
		return 0, errors.New("empty cost")
	}

	negative := false
	digits := text
	switch digits[0] {
	case '-':
		negative = true
		digits = digits[1:]
	case '+':
		digits = digits[1:]
	}
	if digits == "" {
		return 0, fmt.Errorf("cost %q has no digits", text)
	}

	value := 0
	for i := 0; i < len(digits); i++ {
		c := digits[i]
		if c < '0' || c > '9' {
			return 0, fmt.Errorf("cost %q is not an integer", text)
		}
		value = value*10 + int(c-'0')
	}
	if negative {
		value = -value
	}
	return value, nil
}

// writeIndex emits a gzip TSV sorted by reading.  Sorting lets the loader keep
// the file on disk and stream it, and makes the output byte-for-byte
// reproducible for the same dictionary revision.
func writeIndex(path string, readings []string, byReading map[string][]candidate) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	file, err := os.Create(path)
	if err != nil {
		return err
	}
	defer file.Close()

	writer, err := gzip.NewWriterLevel(file, gzip.BestCompression)
	if err != nil {
		return err
	}
	buffered := bufio.NewWriterSize(writer, 1<<20)

	for _, reading := range readings {
		candidates := byReading[reading]
		sort.SliceStable(candidates, func(i, j int) bool {
			return candidates[i].cost < candidates[j].cost
		})
		if _, err := io.WriteString(buffered, reading); err != nil {
			return err
		}
		for _, c := range candidates {
			if err := buffered.WriteByte('\t'); err != nil {
				return err
			}
			if _, err := io.WriteString(buffered, c.surface); err != nil {
				return err
			}
		}
		if err := buffered.WriteByte('\n'); err != nil {
			return err
		}
	}

	if err := buffered.Flush(); err != nil {
		return err
	}
	return writer.Close()
}
