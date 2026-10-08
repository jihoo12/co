package main

import "fmt"

func main() {
	m := map[int]int{}
	n := 2000000
	for i := 0; i < n; i++ {
		m[i*7919%n] = i
	}
	hits := 0
	for round := 0; round < 5; round++ {
		for i := 0; i < n; i++ {
			hits += m[i+round]
		}
	}
	for i := 0; i < n/2; i++ {
		delete(m, i)
	}
	fmt.Println(len(m), hits)
}
