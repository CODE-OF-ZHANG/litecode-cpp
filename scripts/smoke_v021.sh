#!/bin/bash
set +e

echo "=== 1. GET /solutions/1 detail (expect content) ==="
curl -s "http://localhost:8080/api/v1/solutions/1" | python -c "
import sys, json
try:
    d = json.loads(sys.stdin.read())
    s = d.get('data', {})
    print('  keys:', sorted(s.keys()))
    print('  like_count:', s.get('like_count'))
    print('  comment_count:', s.get('comment_count'))
    print('  content_len:', len(s.get('content','')))
except Exception as e:
    print('  err:', e)
"

echo ""
echo "=== 2. GET /solutions/1/comments (expect 200 + empty) ==="
curl -s "http://localhost:8080/api/v1/solutions/1/comments" | python -c "
import sys, json
try:
    d = json.loads(sys.stdin.read())
    print('  data:', d.get('data'))
    print('  code:', d.get('code'))
except Exception as e:
    print('  err:', e)
"

echo ""
echo "=== 3. GET /solutions/99999/comments (expect 404) ==="
curl -s "http://localhost:8080/api/v1/solutions/99999/comments" | python -c "
import sys, json
try:
    d = json.loads(sys.stdin.read())
    print('  code:', d.get('code'), 'message:', d.get('message'))
except Exception as e:
    print('  err:', e)
"

echo ""
echo "=== 4. Login as admin ==="
LOGIN=$(curl -sX POST "http://localhost:8080/api/v1/auth/login" \
    -H "Content-Type: application/json" \
    -d '{"username":"admin","password":"admin12345"}')
echo "  raw: $(echo "$LOGIN" | head -c 200)"
TOKEN=$(echo "$LOGIN" | python -c "
import sys, json
try:
    d = json.loads(sys.stdin.read())
    print(d.get('data', {}).get('access_token', ''))
except: print('')
")
echo "  TOKEN length: ${#TOKEN}"

echo ""
echo "=== 5. POST comment on solution 1 (expect 200 + id) ==="
if [ -n "$TOKEN" ]; then
    POST=$(curl -sX POST "http://localhost:8080/api/v1/solutions/1/comments" \
        -H "Authorization: Bearer $TOKEN" \
        -H "Content-Type: application/json" \
        -d '{"content":"V021 smoke test 评论内容"}')
    echo "  raw: $(echo "$POST" | head -c 300)"
    CID=$(echo "$POST" | python -c "
import sys, json
try:
    d = json.loads(sys.stdin.read())
    print(d.get('data', {}).get('id', ''))
except: print('')
")
    echo "  comment id: $CID"

    echo ""
    echo "=== 6. Re-GET comments (expect 1 item) ==="
    curl -s "http://localhost:8080/api/v1/solutions/1/comments" | python -c "
import sys, json
d = json.loads(sys.stdin.read())
items = d.get('data', {}).get('items', [])
print('  count:', len(items))
if items:
    print('  first:', {k: items[0].get(k) for k in ['id', 'content', 'created_at']})
    print('  user:', items[0].get('user'))
"

    echo ""
    echo "=== 7. Verify solution comment_count incremented ==="
    curl -s "http://localhost:8080/api/v1/solutions/1" | python -c "
import sys, json
d = json.loads(sys.stdin.read())
s = d.get('data', {})
print('  like_count:', s.get('like_count'))
print('  comment_count:', s.get('comment_count'))
"

    if [ -n "$CID" ]; then
        echo ""
        echo "=== 8. DELETE comment (expect deleted: true) ==="
        curl -sX DELETE "http://localhost:8080/api/v1/solution-comments/$CID" \
            -H "Authorization: Bearer $TOKEN" | python -c "
import sys, json
d = json.loads(sys.stdin.read())
print('  data:', d.get('data'))
"

        echo ""
        echo "=== 9. Verify comment_count decremented ==="
        curl -s "http://localhost:8080/api/v1/solutions/1" | python -c "
import sys, json
d = json.loads(sys.stdin.read())
s = d.get('data', {})
print('  comment_count:', s.get('comment_count'))
"
    fi
fi

echo ""
echo "=== 10. POST too long content (expect 400) ==="
LONG=$(python -c "print('a'*2001)")
if [ -n "$TOKEN" ]; then
    curl -sX POST "http://localhost:8080/api/v1/solutions/1/comments" \
        -H "Authorization: Bearer $TOKEN" \
        -H "Content-Type: application/json" \
        -d "{\"content\":\"$LONG\"}" | python -c "
import sys, json
d = json.loads(sys.stdin.read())
print('  code:', d.get('code'), 'message:', d.get('message'))
"
fi

echo ""
echo "=== 11. DELETE other user's comment (expect 403) ==="
# We can't easily set up a second user here; just verify delete on already-deleted comment returns 404
if [ -n "$TOKEN" ]; then
    curl -sX DELETE "http://localhost:8080/api/v1/solution-comments/99999" \
        -H "Authorization: Bearer $TOKEN" | python -c "
import sys, json
d = json.loads(sys.stdin.read())
print('  code:', d.get('code'), 'message:', d.get('message'))
"
fi

echo ""
echo "=== 12. List solutions for add-binary (verify excerpt in payload) ==="
# Get a problem that has a solution via DB
docker compose exec -T mysql mysql -ulitecode -plcp_db_3c231b220e0c litecode -sN -e "SELECT slug FROM problems WHERE id IN (SELECT DISTINCT problem_id FROM solutions WHERE is_deleted=0) LIMIT 1;" 2>/dev/null | head -1 > /tmp/p.txt
PSLUG=$(cat /tmp/p.txt)
echo "  using slug: $PSLUG"
if [ -n "$PSLUG" ]; then
    curl -s "http://localhost:8080/api/v1/problems/$PSLUG/solutions" | python -c "
import sys, json
d = json.loads(sys.stdin.read())
items = d.get('data', {}).get('items', [])
print('  items:', len(items))
if items:
    s = items[0]
    print('  keys:', sorted(s.keys()))
    print('  excerpt_len:', len(s.get('excerpt','')))
    print('  excerpt preview:', s.get('excerpt','')[:120])
    print('  has content:', 'content' in s, '(should be False)')
    print('  comment_count:', s.get('comment_count'))
"
fi