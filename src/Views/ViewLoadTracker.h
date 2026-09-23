#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace OSFUI
{
    enum class ViewLoadState
    {
        Loading,
        Finished,
        Failed
    };

    class ViewLoadTracker
    {
    public:
        void BeginLoad(std::string_view a_id);
        void FinishLoad(std::string_view a_id, bool a_failed);

        ViewLoadState GetState(std::string_view a_id) const;

    private:
        std::unordered_map<std::string, ViewLoadState> m_states;
    };
}
