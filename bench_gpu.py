import torch
import time

def bench_matmul(size, iterations):
    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"Device: {device}")
    
    a = torch.randn(size, size, device=device)
    b = torch.randn(size, size, device=device)
    
    print(f"--- Python (PyTorch) GPU Benchmark (MatMul {size}x{size}) ---")
    print(f"Warmup (5 iterations)...")
    for _ in range(5):
        c = torch.matmul(a, b)
        if device == "cuda":
            torch.cuda.synchronize()
            
    print(f"Measuring {iterations} iterations...")
    start = time.time()
    for _ in range(iterations):
        c = torch.matmul(a, b)
    if device == "cuda":
        torch.cuda.synchronize()
    end = time.time()
    
    total = end - start
    avg = total / iterations
    print(f"Total time: {total:.4f} seconds")
    print(f"Avg time:   {avg * 1000.0:.4f} milliseconds per matmul")
    print("--------------------------------------------------")

if __name__ == "__main__":
    bench_matmul(512, 1000)
