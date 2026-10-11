import { Link, createFileRoute } from "@tanstack/react-router"
import { BoardFrame } from "@/components/board-frame"
import { BoardCard, Disclosure, Panel } from "@/components/store"
import { appBySlug, appsForBoard } from "@/data/apps"
import type { Board } from "@/data/boards"
import { boards } from "@/data/boards"

export const Route = createFileRoute("/")({ component: Home })

const heroSrc = (b: Board) => appBySlug(b.heroApp)?.screens.get(b.slug) ?? null

function Home() {
  return (
    <>
      <h1 className="font-heading text-3xl font-bold tracking-tight md:text-[40px] md:leading-[1.1]">
        Which board do you have?
      </h1>
      <p className="mt-2 text-muted-foreground">
        Drawn to true relative size, each running a real app.
      </p>

      <Panel className="mt-6 px-4 pt-6 pb-5 md:px-10 md:pt-8">
        {/* Zoey: 2.3 px/mm on desktop; a scroll-snapped row at 1.2 below xl, where 2.3 no longer fits. */}
        <Picker scale={2.3} className="hidden xl:block" />
        <Picker scale={1.2} className="xl:hidden" />
      </Panel>

      <h2 className="mt-12 mb-4 font-heading text-2xl font-bold tracking-tight">
        All boards
      </h2>
      <div className="grid gap-3.5 sm:grid-cols-2 lg:grid-cols-3">
        {boards.map((b) => (
          <BoardCard
            key={b.slug}
            board={b}
            appCount={appsForBoard(b.slug).length}
            heroSrc={heroSrc(b)}
          />
        ))}
      </div>
      <Disclosure className="mt-4" />
    </>
  )
}

function Picker({ scale, className }: { scale: number; className?: string }) {
  return (
    <div className={className}>
      <ul className="flex snap-x snap-mandatory items-end gap-6 overflow-x-auto border-b-2 border-dashed pb-4 xl:justify-center xl:gap-8 xl:overflow-visible">
        {boards.map((b) => (
          <li key={b.slug} className="snap-center">
            <Link
              to="/boards/$board"
              params={{ board: b.slug }}
              className="flex min-w-24 flex-col items-center justify-end gap-3.5 rounded-xl px-3.5 pt-3.5 pb-2.5 outline-offset-6 hover:outline-2 hover:outline-primary/40"
            >
              <BoardFrame board={b} src={heroSrc(b)} scale={scale} />
              <span className="text-center">
                <b className="block font-heading text-[15px] font-semibold">
                  {b.short}
                </b>
                <small className="font-mono text-xs whitespace-nowrap text-muted-foreground">
                  {b.maker} · {appsForBoard(b.slug).length} apps
                </small>
              </span>
            </Link>
          </li>
        ))}
      </ul>
      <div className="mt-4 flex flex-wrap items-center gap-2.5 font-mono text-xs text-muted-foreground">
        <span>scale</span>
        <i
          aria-hidden
          className="block h-2 border-2 border-t-0 border-muted-foreground"
          style={{ width: 50 * scale }}
        />
        <span>50 mm</span>
        <span className="ml-auto">
          Don’t have one yet? Every board links to Amazon.
        </span>
      </div>
    </div>
  )
}
