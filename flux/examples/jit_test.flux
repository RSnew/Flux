func factorial(n) {
    if n <= 1 {
        return 1
    }
    return n * factorial(n - 1)
}

print("f(1)=" + str(factorial(1)))
print("f(2)=" + str(factorial(2)))
print("f(3)=" + str(factorial(3)))
print("f(5)=" + str(factorial(5)))
