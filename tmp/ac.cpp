#include <bits/stdc++.h>
using namespace std;
int main() {
    vector<int> a;
    int x;
    while (cin >> x) a.push_back(x);
    if (a.empty()) return 1;
    int target = a.back();
    a.pop_back();
    int n = (int)a.size();
    unordered_map<int,int> seen;
    for (int i = 0; i < n; i++) {
        int need = target - a[i];
        auto it = seen.find(need);
        if (it != seen.end()) {
            cout << it->second << " " << i << "\n";
            return 0;
        }
        seen[a[i]] = i;
    }
    return 0;
}
