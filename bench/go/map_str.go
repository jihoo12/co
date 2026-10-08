package main

import (
	"fmt"
	"strconv"
)

func main() {
	words := []string{}
	for i := 0; i < 200000; i++ {
		words = append(words, "w"+strconv.Itoa(i%50000))
	}
	counts := map[string]int{}
	for round := 0; round < 20; round++ {
		for _, w := range words {
			counts[w]++
		}
	}
	total := 0
	for _, c := range counts {
		total += c
	}
	fmt.Println(len(counts), total)
}
