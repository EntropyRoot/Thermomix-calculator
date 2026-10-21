#!/bin/sh
# tcalc test suite:  sh tests/run_tests.sh ./tcalc
BIN=${1:-./tcalc}
pass=0
fail=0

# check EXPR EXPECTED [options...]
check() {
    expr=$1 want=$2
    shift 2
    got=$("$BIN" -n "$@" -e "$expr" 2>&1)
    if [ "$got" = "$want" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL: %s\n  expected: %s\n  got:      %s\n' "$expr" "$want" "$got"
    fi
}

# check_err EXPR : must fail with an error message
check_err() {
    if "$BIN" -n -e "$1" >/dev/null 2>&1; then
        fail=$((fail + 1))
        printf 'FAIL: %s should be an error\n' "$1"
    else
        pass=$((pass + 1))
    fi
}

# arithmetic and precedence
check '1+2*3' 7
check '2^3^2' 512
check '-2^2' -4
check '2^-1' 0.5
check '(1+2)(3+4)' 21
check '2pi' 6.28318530717959
check '3!' 6
check '2^3!' 64
check '-3!' -6
check '10 % 3' 1
check '-7 % 3' 2
check '7 mod 3' 1
check '0.1 + 0.2' 0.3
check '1/3' 0.333333333333333
check '1e3 + .5' 1000.5
check '1_000_000 * 2' 2000000
check '2^53' 9007199254740992
check '1/0' inf
check '0/0' nan
check 'a = 2; b = 3; a*b + a' 8
check 'x = 5; x += 2; x' 7
check 'sqrt(2*(3+4' 3.74165738677394

# input normalization (Persian digits, phone keyboard symbols)
check '۱۲ × ۳ + ۴٫۵' 40.5
check '10 ÷ 4' 2.5
check '√16 + 2²' 8
check 'π' 3.14159265358979
check 'sin(30°)' 0.5

# trigonometry with exact angles
check 'sin(30)' 0.5
check 'cos(60)' 0.5
check 'tan(45)' 1
check 'sin(180)' 0
check 'cos(90)' 0
check 'tan(90)' nan
check 'asin(1)' 90
check 'sin(pi)' 0 -r
check 'cos(pi/3)' 0.5 -r
check 'atan2(1, 1)' 0.785398163397448 -r
check 'sin(100)' 1 -g
check 'sin(90 deg)' 1 -r

# powers, logs, roots
check 'log(1000)' 3
check 'log(8, 2)' 3
check 'log(27, 3)' 3
check 'ln(e^2)' 2
check '(-8)^(1/3)' -2
check 'root(-32, 5)' -2
check 'cbrt(27)' 3
check 'hypot(3, 4)' 5

# rounding
check 'round(2.675, 2)' 2.68
check 'round(-2.5)' -3
check 'round(1234.5678, -2)' 1200
check 'floor(-2.5)' -3
check 'frac(3.75)' 0.75

# number theory
check 'gcd(12, 18, 30)' 6
check 'lcm(4, 6, 10)' 60
check 'ncr(52, 5)' 2598960
check 'npr(10, 3)' 720
check 'fib(50)' 12586269025
check 'isprime(1000000007)' 1
check 'isprime(1000000007 * 3)' 0
check 'nextprime(100)' 101
check 'totient(36)' 12
check '20!' 2.43290200817664e18

# statistics and probability
check 'mean(1, 2, 3, 4)' 2.5
check 'median(5, 1, 3, 2)' 2.5
check 'std(2, 4, 4, 4, 5, 5, 7, 9)' 2.1380899352994
check 'stdp(2, 4, 4, 4, 5, 5, 7, 9)' 2
check 'normcdf(0)' 0.5
check 'round(norminv(0.975), 6)' 1.959964
check 'gamma(5)' 24

# calculus
check 'sigma(k^2, k, 1, 10)' 385
check 'product(k, k, 1, 5)' 120
check 'integ(x^2, x, 0, 3)' 9
check 'integ(exp(-x^2), x, -inf, inf)' 1.77245385090552
check 'integ(1/x^2, x, 1, inf)' 1
check 'integ(sin(x), x, 0, pi)' 2 -r
check 'deriv(x^3, x, 2)' 12
check 'deriv(x^3, x, 2, 2)' 12
check 'solve(x^2 - 2, x)' 1.4142135623731
check 'solve(x^2 = 9, x, 1)' 3
check 'solve(x^2 - 2x + 1, x)' 1
check 'solve(cos(x) - x, x, 0, 1)' 0.739085133215161 -r
check 'solve(exp(x) = 10, x)' 2.30258509299405

# user functions
check 'f(x) = x^2 + 1; f(3)' 10
check 'g(a, b) = a*b; g(4, 5)' 20
check 'fact2(n) = if(n <= 1, 1, n * fact2(n - 1)); fact2(10)' 3628800
check 'f(x) = sigma(k*x, k, 1, 3); f(2)' 12

# user variables may shadow built-in names
check 'beta = 2; gamma = 3; beta * gamma + gamma(4)' 12
check 'diff = 10 - 4; diff * 2' 12
check_err 'e = 5'
check_err 'pi = 3'

# programmer
check '0xFF + 0b1010 + 0o17' 280
check '5 xor 3' 6
check '6 & 3' 2
check '6 | 3' 7
check '1 << 10' 1024
check '-16 >> 2' -4
check '~5' -6
check '255' 0xFF -x
check '10' 0b1010 --bin
check 'popcount(255)' 8

# logic
check '3 == 3.0000000000001' 1
check '1 < 2 && 2 < 3' 1
check '!0' 1
check 'if(2 > 1, 10, 20)' 10

# units
check 'conv(100, C, F)' 212
check 'conv(1, mi, km)' 1.609344
check 'conv(1, GiB, MB)' 1073.741824
check 'conv(180, deg, rad)' 3.14159265358979

# output formats
check 'pi' 3.14 -e ':fix 2'
check '123456' 1.235e5 -e ':sci 3'
check '123456' 123.46e3 -e ':eng 2'
check '1234567.5' 1,234,567.5 -e ':group'
check '2/3' 0.66667 -p 5

# errors
check_err 'foo + 1'
check_err '2 +'
check_err '2 3'
check_err 'sin(1, 2)'
check_err '5 & 2.5'
check_err 'conv(1, km, kg)'
check_err 'sigma(k, k, 1, 1e10)'
check_err 'f(x) = x; f(1, 2)'

# pipe mode and plotting
got=$(printf 'x = 2\nx * 21\n' | "$BIN" -n)
if [ "$got" = 42 ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: pipe mode got '$got'"; fi
if "$BIN" -n --ascii --no-color ':plot x^2 [-2, 2]' | grep -q '\*'; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
    echo "FAIL: plot output"
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
