// memdemo is a tiny program that intentionally allocates several long-lived
// chunks of memory via distinct call paths, then dumps a heap pprof file to
// disk so it can be uploaded to a Pyroscope server with curl.
//
// Usage:
//
//	go run . [output.pprof]
//
// The default output path is ./heap.pprof.
package main

import (
	"fmt"
	"os"
	"runtime"
	"runtime/pprof"
)

var sink [][]byte

func allocateBigBuffers(count, size int) {
	for i := 0; i < count; i++ {
		buf := make([]byte, size)
		for j := 0; j < size; j += 4096 {
			buf[j] = byte(i)
		}
		sink = append(sink, buf)
	}
}

func allocateManySmallBuffers(count, size int) {
	for i := 0; i < count; i++ {
		buf := make([]byte, size)
		buf[0] = byte(i)
		sink = append(sink, buf)
	}
}

func buildStringIndex(words int) {
	idx := make(map[string][]byte, words)
	for i := 0; i < words; i++ {
		key := fmt.Sprintf("token-%08d", i)
		idx[key] = []byte(key + "-payload-payload-payload-payload")
	}
	sink = append(sink, []byte(fmt.Sprintf("index-of-%d-words", len(idx))))
	for _, v := range idx {
		sink = append(sink, v)
	}
}

func deepCaller(depth, count, size int) {
	if depth == 0 {
		allocateBigBuffers(count, size)
		return
	}
	deepCaller(depth-1, count, size)
}

func main() {
	out := "heap.pprof"
	if len(os.Args) > 1 {
		out = os.Args[1]
	}

	allocateBigBuffers(20, 4*1024*1024)
	allocateManySmallBuffers(50_000, 1024)
	buildStringIndex(20_000)
	deepCaller(8, 5, 2*1024*1024)

	runtime.GC()

	f, err := os.Create(out)
	if err != nil {
		fmt.Fprintf(os.Stderr, "create %s: %v\n", out, err)
		os.Exit(1)
	}
	defer f.Close()

	if err := pprof.Lookup("heap").WriteTo(f, 0); err != nil {
		fmt.Fprintf(os.Stderr, "write heap profile: %v\n", err)
		os.Exit(1)
	}

	var ms runtime.MemStats
	runtime.ReadMemStats(&ms)
	fmt.Printf("heap profile written to %s\n", out)
	fmt.Printf("  HeapAlloc   = %.2f MiB\n", float64(ms.HeapAlloc)/1024/1024)
	fmt.Printf("  HeapInuse   = %.2f MiB\n", float64(ms.HeapInuse)/1024/1024)
	fmt.Printf("  TotalAlloc  = %.2f MiB\n", float64(ms.TotalAlloc)/1024/1024)
	fmt.Printf("  NumGC       = %d\n", ms.NumGC)
}
