import { SELF } from "cloudflare:test";
import { describe, expect, it } from "vitest";

describe("invite landing page (Mini App)", () => {
  for (const path of ["/", "/app"]) {
    it(`serves the landing page at ${path}`, async () => {
      const res = await SELF.fetch(`https://example.com${path}?tgWebAppStartParam=abc`);
      expect(res.status).toBe(200);
      expect(res.headers.get("content-type")).toContain("text/html");
      const html = await res.text();
      expect(html).toContain("Listen together on Tunegram");
      expect(html).toContain("tunegram://join");
      expect(html).toContain("https://github.com/adilabdu/tunegram/releases");
      expect(html).not.toContain("${");
    });
  }

  it("does not serve the page for other methods", async () => {
    const res = await SELF.fetch("https://example.com/app", { method: "POST" });
    expect(res.status).toBe(404);
  });
});
