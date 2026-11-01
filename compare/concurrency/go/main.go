package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"time"
)

const SPAWNS = 2000
const ROUNDS = 100000
const JOBS = 1000000

func spawnJoin() float64 {
	sum := 0
	for i := 0; i < SPAWNS; i++ {
		var wg sync.WaitGroup
		wg.Add(1)
		go func(x int) { _ = x; wg.Done() }(i)
		wg.Wait()
		sum++
	}
	return float64(sum)
}

func spawnMany() float64 {
	sum := 0
	for r := 0; r < 20; r++ {
		var wg sync.WaitGroup
		for i := 0; i < 100; i++ {
			wg.Add(1)
			go func(x int) { _ = x; wg.Done() }(i)
		}
		wg.Wait()
		sum += 100
	}
	return float64(sum)
}

func spawnWork() float64 {
	sum := int64(0)
	for x := int64(0); x < SPAWNS; x++ {
		done := make(chan int64)
		go func(x int64) {
			var a []int64
			for i := int64(0); i < 1000; i++ {
				a = append(a, i+x)
			}
			s := int64(0)
			for _, v := range a {
				s += v
			}
			done <- s
		}(x)
		sum += <-done
	}
	return float64(sum)
}

func pingpong() float64 {
	to := make(chan int64)
	back := make(chan int64)
	go func() {
		for i := 0; i < ROUNDS; i++ {
			p := <-to
			back <- p + 1
		}
	}()
	sum := int64(0)
	for i := int64(0); i < ROUNDS; i++ {
		to <- i
		sum += <-back
	}
	return float64(sum)
}

func fanOut() float64 {
	jobs := make(chan int64, 1024)
	done := make(chan int64)
	for w := 0; w < 3; w++ {
		go func() {
			sum := int64(0)
			for j := range jobs {
				sum += j
			}
			done <- sum
		}()
	}
	for i := int64(0); i < JOBS; i++ {
		jobs <- i
	}
	close(jobs)
	sum := int64(0)
	for w := 0; w < 3; w++ {
		sum += <-done
	}
	return float64(sum)
}

func main() {
	only := ""
	if len(os.Args) > 2 {
		only = os.Args[2]
	}
	for _, b := range []struct {
		name string
		f    func() float64
	}{{"spawnjoin", spawnJoin}, {"spawnmany", spawnMany}, {"spawnwork", spawnWork}, {"pingpong", pingpong}, {"fanout", fanOut}} {
		if only != "" && only != b.name {
			continue
		}
		t0 := time.Now()
		ck := b.f()
		fmt.Printf("bench\t%s\t%v\t%v\n", b.name, time.Since(t0).Seconds(), strconv.FormatFloat(ck, 'f', -1, 64))
	}
}
