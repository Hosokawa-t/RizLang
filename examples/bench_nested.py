count = 0
i = 1
while i <= 200:
    j = 1
    while j <= 200:
        k = 1
        while k <= 200:
            count = count + 1
            k = k + 1
        j = j + 1
    i = i + 1
print(count)
