#include <bits/stdc++.h>
using namespace std;
int main() {
    vector<int> a;
    int x;
    while (cin >> x) a.push_back(x);
    cerr << "read " << a.size() << " ints\n";
    for (size_t i = 0; i < a.size(); i++) cerr << "  a[" << i << "]=" << a[i] << "\n";
    if (a.empty()) return 1;
    int target = a.back();
    a.pop_back();
    int n = (int)a.size();
    cerr << "target=" << target << " n=" << n << "\n";
    unordered_map<int,int> seen;
    for (int i = 0; i < n; i++) {
        int need = target - a[i];
        auto it = seen.find(need);
        cerr << "i=" << i << " need=" << need << " found=" << (it != seen.end()) << "\n";
        if (it != seen.end()) {
            cout << it->second << " " << i << "\n";
            return 0;
        }
        seen[a[i]] = i;
    }
    cerr << "no pair found\n";
    return 0;
}
