import { Link, createFileRoute, notFound } from "@tanstack/react-router"
import { BoardFrame } from "@/components/board-frame"
import { AmazonLink, Panel, Price } from "@/components/store"
import { Button } from "@/components/ui/button"
import { appBySlug } from "@/data/apps"
import { boards, fitScale } from "@/data/boards"
import { cn } from "@/lib/utils"

export const Route = createFileRoute("/apps/$app")({
  validateSearch: (search: Record<string, unknown>): { board?: string } =>
    typeof search.board === "string" ? { board: search.board } : {},
  loader: ({ params }) => {
    const app = appBySlug(params.app)
    if (!app) throw notFound()
    return { app }
  },
  head: ({ loaderData }) => ({
    meta: [{ title: `${loaderData?.app.name} · Flashcart` }],
  }),
  component: AppPage,
})

function AppPage() {
  const { app } = Route.useLoaderData()
  const search = Route.useSearch()
  // ?board= picks the board; an unknown or unsupported one falls back to the first that runs it.
  const board =
    boards.find((b) => b.slug === search.board && app.screens.has(b.slug)) ??
    boards.find((b) => app.screens.has(b.slug))!
  const src = app.screens.get(board.slug) ?? null
  const alt = `${app.name} running on ${board.short}`

  return (
    <Panel className="mx-auto max-w-4xl p-4 md:p-7">
      <div className="flex items-start justify-between gap-4">
        <div>
          <p className="font-mono text-[13px] text-muted-foreground">
            <Link to="/apps" className="hover:text-foreground">
              Apps
            </Link>{" "}
            / {app.name}
          </p>
          <h1 className="mt-1 font-heading text-3xl font-bold tracking-tight md:text-[40px] md:leading-[1.1]">
            {app.name}
          </h1>
          <p className="text-muted-foreground">{app.tagline}</p>
        </div>
        <Price price={app.price} className="text-sm" />
      </div>

      <h2 className="sr-only">Runs on</h2>
      <ul className="my-5 grid grid-cols-3 gap-2 sm:grid-cols-5">
        {boards.map((b) => {
          const screen = app.screens.get(b.slug)
          const mini = (
            <>
              <span className="grid h-10 place-items-end">
                <BoardFrame
                  board={b}
                  src={screen ?? null}
                  scale={fitScale(b.frame, 40, 40)}
                  shadow={false}
                />
              </span>
              {b.short}
              {!screen && <span>not yet</span>}
            </>
          )
          const seg =
            "flex flex-1 flex-col items-center gap-1.5 rounded-[10px] border px-1.5 pt-2.5 pb-2 text-center font-mono text-xs font-medium text-muted-foreground"
          return (
            <li key={b.slug} className="flex">
              {screen ? (
                <Link
                  to="/apps/$app"
                  params={{ app: app.slug }}
                  search={{ board: b.slug }}
                  replace
                  aria-current={b === board ? "true" : undefined}
                  className={cn(
                    seg,
                    "hover:border-primary",
                    b === board &&
                      "border-2 border-primary bg-accent text-foreground"
                  )}
                >
                  {mini}
                </Link>
              ) : (
                <div className={cn(seg, "border-dashed opacity-45")}>
                  {mini}
                </div>
              )}
            </li>
          )
        })}
      </ul>

      <div className="relative grid h-[340px] place-items-center overflow-hidden rounded-xl stage md:h-[440px]">
        {/* Desktop and mobile get their own scale, like the home picker. */}
        <div className="hidden md:block">
          <BoardFrame
            board={board}
            src={src}
            scale={fitScale(board.frame, 680, 360)}
            alt={alt}
          />
        </div>
        <div className="md:hidden">
          <BoardFrame
            board={board}
            src={src}
            scale={fitScale(board.frame, 280, 280)}
            alt={alt}
          />
        </div>
        <p className="absolute right-3.5 bottom-3 font-mono text-xs text-muted-foreground">
          {app.name} on {board.maker} {board.short} · {board.res}
        </p>
      </div>

      <div className="mt-4 flex flex-wrap items-center justify-between gap-3">
        {/* ponytail: flashing from the browser isn't built yet; the button holds its place. */}
        <Button size="xl" disabled>
          Flash to my board
        </Button>
        <p className="text-[13px] text-muted-foreground">
          Need this board?{" "}
          <AmazonLink
            href={board.amazon.ca}
            className="font-semibold text-primary hover:underline"
          >
            Check price on Amazon ↗
          </AmazonLink>{" "}
          ·{" "}
          <Link
            to="/boards/$board"
            params={{ board: board.slug }}
            className="font-semibold text-primary hover:underline"
          >
            About the {board.short}
          </Link>
        </p>
      </div>
    </Panel>
  )
}
