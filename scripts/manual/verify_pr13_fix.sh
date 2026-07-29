#!/bin/bash
set -e
TOKEN=$(curl -sS -X POST http://127.0.0.1:8080/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"changeme123"}' \
  | sed -n 's/.*"access_token":"\([^"]*\)".*/\1/p')
echo "TOKEN ok: ${#TOKEN}"

# 用 printf 严格控制字节:JSON 文件里必须字面是 \n (2 chars),
# JSON 解码后字符串里也是字面 \n (2 chars),C++ 编译时 char literal '\n' = newline。
printf '%s' '{"problem_id":17,"language":"cpp","code":"#include <bits/stdc++.h>\nusing namespace std;\nint main(){int n;cin>>n;vector<int>a(n);for(int&x:a)cin>>x;int j=0;for(int i=0;i<n;i++)if(a[i])a[j++]=a[i];fill(a.begin()+j,a.end(),0);for(int i=0;i<n;i++)cout<<a[i]<<(i+1==n?char(10):char(32));return 0;}"}' > /tmp/run.json

echo "=== JSON (cat -A) ==="
cat -A /tmp/run.json
echo
echo "=== RUN ==="
curl -sS -X POST http://127.0.0.1:8080/api/v1/submissions/run-samples \
  -H "Authorization: Bearer $TOKEN" \
  -H "Content-Type: application/json" \
  --data @/tmp/run.json --max-time 60
echo
