#include "graph.h"

#include <cassert>
#include <map>
#include <queue>

#include <tbb/parallel_for.h>

#include "bfs_visitors.h"

namespace lightfields {

Graph::Graph(const V2i& size, int n_link_value, std::size_t layer_count) : m_size(size) {
	m_tLinks = std::vector<TLinks>(layer_count, TLinks(size));
	m_nLinks = std::vector<NLinks>(layer_count, NLinks(size, n_link_value));
}

void Graph::setValue(const V2i& pos, const std::vector<int>& values) {
	assert(values.size() == m_tLinks.size());

	auto tit = m_tLinks.begin();
	auto vit = values.begin();

	int minFlow = values.front();

	while(tit != m_tLinks.end()) {
		tit->edge(pos).setCapacity(*vit);

		assert(*vit >= 0);
		assert(tit->edge(pos).residualCapacity() == *vit);

		minFlow = std::min(minFlow, *vit);

		++tit;
		++vit;
	}

	// speedup of the iterations - addresses the "trivial cases" removing the direct column
	// flow from source to sink
	if(minFlow > 0)
		for(tit = m_tLinks.begin(); tit != m_tLinks.end(); ++tit)
			tit->edge(pos).addFlow(minFlow);
}

std::size_t Graph::v2i(const V2i& v) const {
	return v.x + v.y*m_size.x;
}

namespace {

struct QueueItem {
	Index current, parent;
};

}

bool Graph::iterate(const tbb::blocked_range2d<int>& range) {
	bool foundPath = false;

	BFSVisitors visited(range, m_nLinks.size());
	std::deque<QueueItem> q;

	// initialize - source index has parent -1,-1
	for(int y=range.rows().begin(); y != range.rows().end(); ++y)
		for(int x=range.cols().begin(); x != range.cols().end(); ++x) {
			const Index src_v{V2i(x, y), 0};

			visited.visit(src_v, Index{V2i(-1, -1), 0});
			q.push_back(QueueItem{src_v, Index{V2i(-1, -1), 0}});
		}

	// the core of the algorithm
	while(!q.empty()) {
		QueueItem item = q.front();
		q.pop_front();

		Index current_v = item.current;

		// a potential exit point
		if(current_v.n_layer == m_nLinks.size()-1) {
			if(m_tLinks.back().edge(current_v.pos).residualCapacity() > 0) {
				foundPath = true;

				const int f = flow(visited, current_v);
				if(f > 0)
					doFlow(visited, current_v, f);
			}
		}

		// try to move horizontally left
		if(current_v.pos.x > range.cols().begin()) {
			const Index new_v{V2i(current_v.pos.x-1, current_v.pos.y), current_v.n_layer};
			if(!visited.visited(new_v) && m_nLinks[current_v.n_layer].edge(current_v.pos, new_v.pos).residualCapacity() > 0) {
				visited.visit(new_v, current_v);
				q.push_back(QueueItem{new_v, current_v});
			}
		}

		// try to move horizontally right
		if(current_v.pos.x < range.cols().end()-1){
			const Index new_v{V2i(current_v.pos.x+1, current_v.pos.y), current_v.n_layer};
			if(!visited.visited(new_v) && m_nLinks[current_v.n_layer].edge(current_v.pos, new_v.pos).residualCapacity() > 0) {
				visited.visit(new_v, current_v);
				q.push_back(QueueItem{new_v, current_v});
			}
		}

		// try to move vertically up
		if(current_v.pos.y > range.rows().begin()) {
			const Index new_v{V2i(current_v.pos.x, current_v.pos.y-1), current_v.n_layer};
			if(!visited.visited(new_v) && m_nLinks[current_v.n_layer].edge(current_v.pos, new_v.pos).residualCapacity() > 0) {
				visited.visit(new_v, current_v);
				q.push_back(QueueItem{new_v, current_v});
			}
		}

		// try to move vertically down
		if(current_v.pos.y < range.rows().end()-1) {
			const Index new_v{V2i(current_v.pos.x, current_v.pos.y+1), current_v.n_layer};
			if(!visited.visited(new_v) && m_nLinks[current_v.n_layer].edge(current_v.pos, new_v.pos).residualCapacity() > 0) {
				visited.visit(new_v, current_v);
				q.push_back(QueueItem{new_v, current_v});
			}
		}

		// try to move up a layer (unconditional)
		if((current_v.n_layer > 0)) {
			const Index new_v{current_v.pos, current_v.n_layer-1};
			if(!visited.visited(new_v)) {
				visited.visit(new_v, current_v);
				q.push_back(QueueItem{new_v, current_v});
			}
		}

		// try to move down a layer
		if((current_v.n_layer < m_nLinks.size()-1)) {
			const Index new_v{current_v.pos, current_v.n_layer+1};
			if(!visited.visited(new_v) && m_tLinks[current_v.n_layer].edge(current_v.pos).residualCapacity() > 0) {
				visited.visit(new_v, current_v);
				q.push_back(QueueItem{new_v, current_v});
			}
		}
	}

	return foundPath;
}

int Graph::flow(const BFSVisitors& visitors, const Index& end) const {
	assert(end.pos.x != -1 && end.pos.y != -1);
	assert(end.n_layer == m_nLinks.size()-1);

	// initial flow value based on the last T link (towards the sink)
	int result = m_tLinks.back().edge(end.pos).residualCapacity();

	Index current = end;
	Index parent = visitors.parent(current);

	while(result > 0 && parent.pos.x != -1 && parent.pos.y != -1) {
		assert(Index::sqdist(current, parent) == 1);

		// same layer - need to use N link
		if(current.n_layer == parent.n_layer)
			result = std::min(result, m_nLinks[current.n_layer].edge(parent.pos, current.pos).residualCapacity());

		// different layer down - need to use a T link (if the move is to higher layer)
		else if(current.n_layer > parent.n_layer)
			result = std::min(result, m_tLinks[parent.n_layer].edge(parent.pos).residualCapacity());

		// different layer up - "infinite capacity" back links have no impact on the flow

		// and move on
		current = parent;
		parent = visitors.parent(current);
	}

	assert(current.n_layer == 0 || result == 0);

	return result;
}

void Graph::doFlow(const BFSVisitors& visitors, const Index& end, int flow) {
	assert(end.pos.x != -1 && end.pos.y != -1);
	assert(end.n_layer == m_nLinks.size()-1);

	// initial flow value based on the last T link (towards the sink)
	m_tLinks.back().edge(end.pos).addFlow(flow);


	Index current = end;
	Index parent = visitors.parent(current);

	while(parent.pos.x != -1 && parent.pos.y != -1) {
		assert(Index::sqdist(current, parent) == 1);

		// same layer - need to use N link
		if(current.n_layer == parent.n_layer)
			m_nLinks[current.n_layer].edge(parent.pos, current.pos).addFlow(flow);

		// different layer down - need to use a T link (if the move is to higher layer)
		else if(current.n_layer > parent.n_layer)
			m_tLinks[parent.n_layer].edge(parent.pos).addFlow(flow);

		// different layer up - "infinite capacity" back links have no impact on the flow

		// and move on
		current = parent;
		parent = visitors.parent(current);
	}

	assert(current.n_layer == 0);
}

void Graph::solve() {
	std::size_t counter = 0;

	// split to tiles and process automagically
	bool go_on = true;
	while(go_on) {
		go_on = false;

		tbb::parallel_for(
			tbb::blocked_range2d<int>(0, m_size.y, 0, m_size.x),
			[&](const tbb::blocked_range2d<int>& range) {
				if(iterate(range))
					go_on = true;
			}
		);

		++counter;
	}

	// connect the tiles
	while(iterate(tbb::blocked_range2d<int>(0, m_size.y, 0, m_size.x)))
		++counter;

	std::cout << "ST-cut solve with " << counter << " iterations finished." << std::endl;
}

cv::Mat Graph::minCut() const {
	// collects all reachable nodes from source or sink, and marks them appropriately
	cv::Mat result = cv::Mat::zeros(m_size.y, m_size.x, CV_8UC1);

	// for(int y=0; y<m_size.y; ++y)
	// 	for(int x=0; x<m_size.x; ++x) {
	// 		// int a=0;
	// 		for(std::size_t a=0; a<m_tLinks.size(); ++a)
	// 			if(m_tLinks[a].edge(V2i(x, y)).residualCapacity() == 0)
	// 				result.at<unsigned char>(y, x) = a * (255 / (m_tLinks.size() - 1));
	// 	}

	// reachable from source as a trivial DFS search
	{
		std::vector<bool> visited(m_size.x * m_size.y * (m_nLinks.size()+1));

		std::vector<Index> stack;
		for(int y=0; y<m_size.y; ++y)
			for(int x=0; x<m_size.x; ++x) {
				stack.push_back(Index{V2i(x, y), 0});

				while(!stack.empty()) {
					const Index current = stack.back();
					stack.pop_back();

					unsigned char& value = result.at<unsigned char>(current.pos.y, current.pos.x);
					const int target = (current.n_layer) * (255 / (m_nLinks.size() - 1));

					if(!visited[v2i(current.pos) + current.n_layer * m_size.x * m_size.y]) {
						visited[v2i(current.pos) + current.n_layer * m_size.x * m_size.y] = true;

						if(target > value)
							value = target;

						if(current.n_layer < m_nLinks.size()) {
							// horizontal move
							if(current.pos.x > 0 && m_nLinks[current.n_layer].edge(current.pos, V2i(current.pos.x-1, current.pos.y)).residualCapacity() > 0)
								stack.push_back(Index{V2i(current.pos.x-1, current.pos.y), current.n_layer});
							if(current.pos.x < m_size.x-1 && m_nLinks[current.n_layer].edge(current.pos, V2i(current.pos.x+1, current.pos.y)).residualCapacity() > 0)
								stack.push_back(Index{V2i(current.pos.x+1, current.pos.y), current.n_layer});

							// vertical move
							if(current.pos.y > 0 && m_nLinks[current.n_layer].edge(current.pos, V2i(current.pos.x, current.pos.y-1)).residualCapacity() > 0)
								stack.push_back(Index{V2i(current.pos.x, current.pos.y-1), current.n_layer});
							if(current.pos.y < m_size.y-1 && m_nLinks[current.n_layer].edge(current.pos, V2i(current.pos.x, current.pos.y+1)).residualCapacity() > 0)
								stack.push_back(Index{V2i(current.pos.x, current.pos.y+1), current.n_layer});

							// layer move down
							assert(current.n_layer < m_nLinks.size());
							if(m_tLinks[current.n_layer].edge(current.pos).residualCapacity() > 0)
								stack.push_back(Index{current.pos, current.n_layer+1});

							// layer move up (unconditional)
							if(current.n_layer > 0)
								stack.push_back(Index{current.pos, current.n_layer-1});
						}
					}
				}
			}
	}

	return result;
}

std::vector<cv::Mat> Graph::debug() const {
	std::vector<cv::Mat> result;
	for(std::size_t a=0;a<m_nLinks.size(); ++a) {
		result.push_back(n_flow(m_nLinks[a]));
		result.push_back(t_flow(m_tLinks[a]));
	}

	return result;
}

cv::Mat Graph::t_flow(const TLinks& t) const {
	cv::Mat result(m_size.y, m_size.x, CV_8UC1);

	for(int y=0; y<m_size.y; ++y)
		for(int x=0; x<m_size.x; ++x) {
			auto& e = t.edge(V2i(x, y));
			if(e.capacity() > 0)
				result.at<unsigned char>(y,x) = std::abs(e.flow()) * 255 / e.capacity();
			else
				result.at<unsigned char>(y,x) = 255;
		}

	return result;
}

cv::Mat Graph::n_flow(const NLinks& n) const {
	cv::Mat result(m_size.y, m_size.x, CV_8UC1);

	for(int y=0; y<m_size.y; ++y)
		for(int x=0; x<m_size.x; ++x) {
			float residual = 0.0f;
			float max = 0.0f;
			if(x > 0) {
				auto& e = n.edge(V2i(x-1, y), V2i(x, y));
				if(e.capacity() > 0) {
					residual += (float)e.residualCapacity();
					max += (float)e.capacity();
				}
			}

			if(x < m_size.x-1) {
				auto& e = n.edge(V2i(x+1, y), V2i(x, y));
				if(e.capacity() > 0) {
					residual += (float)e.residualCapacity();
					max += (float)e.capacity();
				}
			}

			if(y > 0) {
				auto& e = n.edge(V2i(x, y-1), V2i(x, y));
				if(e.capacity() > 0) {
					residual += (float)e.residualCapacity();
					max += (float)e.capacity();
				}
			}

			if(y < m_size.y-1) {
				auto& e = n.edge(V2i(x, y+1), V2i(x, y));
				if(e.capacity() > 0) {
					residual += (float)e.residualCapacity();
					max += (float)e.capacity();
				}
			}

			if(max > 0.0f)
				result.at<unsigned char>(y,x) = 255.0f - residual / max * 255.0f;
			else
				result.at<unsigned char>(y,x) = 255;
		}

	return result;
}

}
