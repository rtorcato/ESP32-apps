// Amazon Associates tracking tag, applied to every Amazon link in the store.
// Placeholder: we don't have the real tag yet. Set it here (e.g. "yourtag-20") and
// nothing else changes. While null, links go to Amazon untagged.
export const AMAZON_ASSOCIATES_TAG: string | null = null

export const AMAZON_DISCLOSURE =
  "As an Amazon Associate we earn from qualifying purchases."

export function amazonUrl(url: string) {
  if (!AMAZON_ASSOCIATES_TAG) return url
  const u = new URL(url)
  u.searchParams.set("tag", AMAZON_ASSOCIATES_TAG)
  return u.toString()
}
