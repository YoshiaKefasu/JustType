// Command znzclient sends one ZNZ1 request down the named pipe the way the
// Mozkey client does, and prints the response.
//
// It exists so the bridge can be verified against a real pipe without running a
// full Mozc build:
//
//	znzclient -reading てんき -left "きょうは いい" -right "ですね"
package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"mozkey-jev-bridge/internal/znz"
)

func main() {
	pipeName := flag.String("pipe", `\\.\pipe\mozc_zenz_scorer`, "named pipe path")
	left := flag.String("left", "", "left context text")
	reading := flag.String("reading", "", "katakana reading for the conversion segment")
	right := flag.String("right", "", "right context text")
	generation := flag.Uint("generation", 1, "generation number echoed back")
	timeout := flag.Uint("timeout", 1500, "request timeout in milliseconds")
	maxOutput := flag.Uint("max-output-chars", 256, "maximum characters in the value")
	repeat := flag.Int("repeat", 1, "how many requests to send")
	corruptMagic := flag.Bool("corrupt-magic", false,
		"corrupt the magic so the bridge answers with status=Error; use this to verify the violation path")
	stall := flag.Duration("stall", 0,
		"connect, hold the pipe open for this long and write nothing; use this to verify the bridge's I/O watchdog")
	flag.Parse()

	if *stall > 0 {
		if err := stallConnection(*pipeName, *stall); err != nil {
			fmt.Fprintf(os.Stderr, "znzclient: %v\n", err)
			os.Exit(1)
		}
		return
	}

	if *reading == "" {
		fmt.Fprintln(os.Stderr, "znzclient: -reading is required")
		os.Exit(2)
	}

	prompt := buildPrompt(*left, *reading, *right)

	for i := 0; i < *repeat; i++ {
		if err := runOnce(*pipeName, prompt, uint32(*generation)+uint32(i),
			uint32(*timeout), uint32(*maxOutput), *corruptMagic); err != nil {
			fmt.Fprintf(os.Stderr, "znzclient: %v\n", err)
			os.Exit(1)
		}
	}
}

// stallConnection connects and deliberately writes nothing.
//
// Without the I/O watchdog the bridge blocks in ReadFile forever, so a healthy
// bridge drops this connection after its safety timeout and keeps serving.
func stallConnection(pipeName string, duration time.Duration) error {
	conn, err := os.OpenFile(pipeName, os.O_RDWR, 0)
	if err != nil {
		return fmt.Errorf("open %s: %w", pipeName, err)
	}
	defer conn.Close()

	fmt.Printf("connected, holding %s without writing\n", duration)
	time.Sleep(duration)
	fmt.Println("closing")
	return nil
}

func buildPrompt(left, reading, right string) string {
	var builder strings.Builder
	builder.WriteString("\uee02")
	builder.WriteString(left)
	if right != "" {
		builder.WriteString("\uee07")
		builder.WriteString(right)
	}
	builder.WriteString("\uee00")
	builder.WriteString(reading)
	builder.WriteString("\uee01")
	return builder.String()
}

// runOnce performs a full connect, request, response, disconnect cycle.
func runOnce(pipeName, prompt string, generation, timeoutMsec, maxOutput uint32,
	corruptMagic bool) error {
	conn, err := os.OpenFile(pipeName, os.O_RDWR, 0)
	if err != nil {
		return fmt.Errorf("open %s: %w", pipeName, err)
	}
	defer conn.Close()

	request := znz.EncodeRequestHeader(znz.RequestHeader{
		Generation:     generation,
		TimeoutMsec:    timeoutMsec,
		MaxOutputChars: maxOutput,
		PromptSize:     uint32(len(prompt)),
	})
	request = append(request, prompt...)

	if corruptMagic {
		request[0] = 0x00
	}
	if _, err := conn.Write(request); err != nil {
		return fmt.Errorf("write request: %w", err)
	}

	header := make([]byte, znz.ResponseHeaderSize)
	if _, err := io.ReadFull(conn, header); err != nil {
		return fmt.Errorf("read response header: %w", err)
	}

	if got := binary.LittleEndian.Uint32(header[0:4]); got != znz.Magic {
		return fmt.Errorf("bad magic %#x", got)
	}

	status := binary.LittleEndian.Uint32(header[12:16])
	latency := binary.LittleEndian.Uint32(header[16:20])
	valueSize := binary.LittleEndian.Uint32(header[20:24])
	debugSize := binary.LittleEndian.Uint32(header[24:28])

	value := make([]byte, valueSize)
	if _, err := io.ReadFull(conn, value); err != nil {
		return fmt.Errorf("read value: %w", err)
	}
	debug := make([]byte, debugSize)
	if _, err := io.ReadFull(conn, debug); err != nil {
		return fmt.Errorf("read debug: %w", err)
	}

	fmt.Printf("gen=%d status=%s latency=%dms value=%q debug=%q\n",
		binary.LittleEndian.Uint32(header[8:12]), statusName(status), latency,
		string(value), string(debug))
	return nil
}

func statusName(status uint32) string {
	switch status {
	case znz.StatusOK:
		return "OK"
	case znz.StatusError:
		return "Error"
	case znz.StatusTimeout:
		return "Timeout"
	default:
		return "Unknown(" + strconv.FormatUint(uint64(status), 10) + ")"
	}
}
