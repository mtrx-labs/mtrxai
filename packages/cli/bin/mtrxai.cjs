#!/usr/bin/env node
require("child_process").execFileSync("bun", [require("path").join(__dirname, "../src/index.ts"), ...process.argv.slice(2)], { stdio: "inherit" })
