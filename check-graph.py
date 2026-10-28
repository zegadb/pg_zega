"""The graph test.sql leaves in Zega, in either zega.mode. Zega at 127.0.0.1:9342."""
import json, urllib.request
graph = json.load(urllib.request.urlopen(
    urllib.request.Request("http://127.0.0.1:9342/graph", headers={"Accept": "application/json", "Authorization": "Bearer pg_zega-test-token"})
))
nodes = graph["result"]["nodes"]
rels = graph["result"]["rels"]
print(json.dumps({"nodes": nodes, "rels": rels}, indent=2))
assert graph["ok"] is True
people = [n for n in nodes if n.get("labels") == ["people"]]
users = {n["user_id"]: n for n in nodes if "user_id" in n}
assert len(people) == 1, people
node = people[0]
assert node["person_id"] == 1
assert node["name"] == 'Ada "Lovelace"'
assert node["email"] == "ada@example.com"
assert node["active"] is True
assert "photo" not in node
assert node.get("nickname") is None, node
assert "Grace" not in json.dumps(nodes)
assert "temp" not in json.dumps(nodes)
assert len(rels) == 1, rels
rel = rels[0]
assert rel["type"] == "friends"
assert rel["from"] == users[10]["id"]
assert rel["to"] == users[11]["id"]
assert rel["props"]["since"] == 2016
assert rel["props"]["note"] == "lab"
assert rel["props"]["met"] is True
print("pg_zega sync test passed")
