package main

import "fmt"

func main() {
	total := 0
	for round := 0; round < 20; round++ {
		nums := []int{}
		for i := 0; i < 5000000; i++ {
			nums = append(nums, i+round)
		}
		for _, n := range nums {
			total += n
		}
	}
	fmt.Println(total)
}
