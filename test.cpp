#pragma GCC optimize("O3")
#pragma GCC optimize("unroll-loops")
#pragma GCC target("avx,avx2,sse,sse2,sse3,sse4,popcnt")

#include <bits/stdc++.h>

using namespace std;
using ll = long long;

int main() {
    cin.tie(nullptr)->sync_with_stdio(false);
    cout << fixed << setprecision(10) << '\n';
    int t = 1;
    cin >> t;
    for (int _t = 1; _t <= t; ++_t) {
        int n;
        cin >> n;
        cout << n << '\n';
    }
    return 0;
}
