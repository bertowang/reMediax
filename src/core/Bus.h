// 数据总线：节点链式串联，节点间用有界队列解耦，并按队列深度做背压
#ifndef TGCAPTURE_CORE_BUS_H
#define TGCAPTURE_CORE_BUS_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Frame.h"

namespace TGCapture::core
{

class BusNode
{
public:
    explicit BusNode(std::string name) : m_name(std::move(name)) {}
    virtual ~BusNode() = default;

    const std::string& Name() const noexcept { return m_name; }

    // 上游提交（入队）。返回 false 表示节点已停止
    virtual bool Submit(FramePtr frame) = 0;

    // 当前积压数量，用于上游背压判断
    virtual size_t Pending() const = 0;

    void LinkNext(std::shared_ptr<BusNode> next) { m_next = std::move(next); }
    std::shared_ptr<BusNode> Next() const noexcept { return m_next; }

protected:
    // 交给下一节点；链尾返回 true 表示流程结束
    bool Forward(FramePtr frame)
    {
        if (!m_next) {
            return true;
        }
        return m_next->Submit(std::move(frame));
    }

private:
    std::string m_name;
    std::shared_ptr<BusNode> m_next;
};

using BusNodePtr = std::shared_ptr<BusNode>;

class Bus
{
public:
    void Register(const std::string& name, BusNodePtr node)
    {
        m_nodes[name] = std::move(node);
    }

    BusNodePtr Get(const std::string& name) const
    {
        auto it = m_nodes.find(name);
        return it == m_nodes.end() ? nullptr : it->second;
    }

    bool Dispatch(const std::string& entry, FramePtr frame)
    {
        auto node = Get(entry);
        return node && node->Submit(std::move(frame));
    }

    size_t TotalPending() const
    {
        size_t total = 0;
        for (const auto& [_, node] : m_nodes) {
            total += node->Pending();
        }
        return total;
    }

    size_t PendingOf(const std::string& name) const
    {
        auto node = Get(name);
        return node ? node->Pending() : 0;
    }

    std::vector<std::string> Dump() const
    {
        std::vector<std::string> lines;
        for (const auto& [name, node] : m_nodes) {
            lines.push_back(name + " pending=" + std::to_string(node->Pending()));
        }
        return lines;
    }

private:
    std::unordered_map<std::string, BusNodePtr> m_nodes;
};

}  // namespace TGCapture::core

#endif  // TGCAPTURE_CORE_BUS_H
