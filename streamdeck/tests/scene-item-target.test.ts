import { strict as assert } from "node:assert";
import { test } from "node:test";
import { decodeItem } from "../src/scene-item-target.ts";
test("scene item control retains the collection, group and item identity", () => {
	const value = JSON.stringify(["Collection", "Group", 7, "Camera", true]);
	assert.deepEqual(decodeItem(value), { collection: "Collection", parent: "Group", id: 7, name: "Camera", group: true });
	for (const bad of ["", "{}", '["","Group",7,"Camera",true]', '["A","B",-1,"C",false]', '["A","B",2,"C",1]'])
		assert.equal(decodeItem(bad), undefined);
});
