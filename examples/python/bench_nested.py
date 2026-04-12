count = 0
i = 1
while i <= 1000:
    j = 1
    while j <= 1000:
        k = 1
        while k <= 200:
            count += 1
            k += 1
        j += 1
    i += 1
print(count)
