/// Stamps a freshly opened/saved project as the most-recently-used and persists it via
/// the control plane. Returns the updated recent-project list so callers that show it
/// (the startup modal) can refresh without a second round-trip.
import { client, type ProjectInfo, type RecentProjects } from "../control/client";

export async function rememberProject(project: ProjectInfo): Promise<RecentProjects> {
  return client.rememberRecentProject({
    path: project.path,
    name: project.name,
    displayName: project.displayName,
    lastOpenedAt: new Date().toISOString(),
  });
}
