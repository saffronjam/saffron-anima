//! The search aggregator: fans a query across the active connectors, holding each
//! source's own cursor and exhaustion, and interleaving results round-robin. There is no
//! pagination and no synthesized global relevance order — the UI's scroll position drives
//! how many batches are pulled.

use std::collections::VecDeque;
use std::sync::Arc;

use super::{SearchQuery, StoreConnector, StoreCursor, StoreResult};

/// Per-connector state carried across `next_batch` calls.
struct Source {
    connector: Arc<dyn StoreConnector>,
    cursor: Option<StoreCursor>,
    exhausted: bool,
    buffer: VecDeque<StoreResult>,
}

impl Source {
    fn drained(&self) -> bool {
        self.exhausted && self.buffer.is_empty()
    }
}

/// One live search session, keyed in the bridge by an id.
pub struct SearchSession {
    query: SearchQuery,
    sources: Vec<Source>,
}

impl SearchSession {
    pub fn new(query: SearchQuery, connectors: Vec<Arc<dyn StoreConnector>>) -> Self {
        let sources = connectors
            .into_iter()
            .map(|connector| Source {
                connector,
                cursor: None,
                exhausted: false,
                buffer: VecDeque::new(),
            })
            .collect();
        Self { query, sources }
    }

    /// Whether every source is exhausted and its buffer drained.
    pub fn all_exhausted(&self) -> bool {
        self.sources.iter().all(Source::drained)
    }

    /// Pulls one page from each non-exhausted source whose buffer is empty.
    async fn refill(&mut self) -> bool {
        let mut fetched = false;
        for source in &mut self.sources {
            if source.exhausted || !source.buffer.is_empty() {
                continue;
            }
            match source
                .connector
                .search(&self.query, source.cursor.clone())
                .await
            {
                Ok(page) => {
                    source.buffer.extend(page.results);
                    source.cursor = page.next_cursor;
                    source.exhausted = page.exhausted;
                    fetched = true;
                }
                Err(err) => {
                    // One source failing must not blank the whole Store; mark it done so
                    // the session stops hammering it, and surface the reason in the log.
                    tracing::warn!(
                        "store connector '{}' search failed: {err}",
                        source.connector.id()
                    );
                    source.exhausted = true;
                }
            }
        }
        fetched
    }

    /// The next round-robin batch of up to `count` results.
    pub async fn next_batch(&mut self, count: usize) -> Vec<StoreResult> {
        self.refill().await;
        let mut out = Vec::with_capacity(count);
        while out.len() < count {
            let mut progressed = false;
            for source in &mut self.sources {
                if out.len() >= count {
                    break;
                }
                if let Some(item) = source.buffer.pop_front() {
                    out.push(item);
                    progressed = true;
                }
            }
            if !progressed {
                // Every buffer is empty — try once more to refill before giving up.
                if !self.refill().await {
                    break;
                }
            }
        }
        out
    }
}
