import { createFileRoute } from "@tanstack/react-router"

// ponytail: placeholder until Zoey's dashboard mockups are signed off.
export const Route = createFileRoute("/dashboard")({ component: Dashboard })

function Dashboard() {
  return (
    <main className="p-6">
      <h1 className="font-medium">Dashboard</h1>
    </main>
  )
}
