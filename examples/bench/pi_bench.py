def calculate_pi(n):
    pi = 0.0
    divisor = 1.0
    for i in range(n):
        if i % 2 == 0:
            pi += 1.0 / divisor
        else:
            pi -= 1.0 / divisor
        divisor += 2.0
    return 4.0 * pi

n = 10000000
print(f"Calculated Pi with {n} iterations...")
result = calculate_pi(n)
print(f"Result: {result}")
