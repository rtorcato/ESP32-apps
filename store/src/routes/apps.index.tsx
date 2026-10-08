import { createFileRoute } from "@tanstack/react-router"
import { AppTile } from "@/components/store"
import { apps } from "@/data/apps"

export const Route = createFileRoute("/apps/")({
  head: () => ({ meta: [{ title: "Apps · Flashcart" }] }),
  component: AppsPage,
})

function AppsPage() {
  return (
    <>
      <h1 className="font-heading text-3xl font-bold tracking-tight md:text-[40px] md:leading-[1.1]">
        Apps
      </h1>
      <p className="mt-2 mb-6 text-muted-foreground">
        {apps.length} apps across every board.
      </p>
      <div className="grid grid-cols-2 gap-3.5 sm:grid-cols-3 lg:grid-cols-6">
        {apps.map((a) => (
          <AppTile key={a.slug} app={a} />
        ))}
      </div>
    </>
  )
}
