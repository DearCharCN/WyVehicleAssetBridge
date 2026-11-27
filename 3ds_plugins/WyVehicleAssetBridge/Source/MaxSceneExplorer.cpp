#include "MaxScene.h"
#include <max.h>
#include <maxscript/maxscript.h>

void WyRevealSelectionInExplorers()
{
    // Find Selected operates on the Explorer's own selection. A locked or
    // not-yet-refreshed Explorer may still be pointing at the previous Max node.
    static constexpr wchar_t script[] = LR"(
        (
            local target = selection[1]
            fn isTarget explorer target = (
                local items = explorer.SelectedItems()
                items.count == 1 and items[1] == target
            )
            fn findTree parent = (
                if (parent.GetType()).FullName == "MaxCustomControls.SceneExplorerControls.SceneExplorerTreeList" do return parent
                for j = 0 to (parent.Controls.Count - 1) do (
                    local childTree = findTree parent.Controls.Item[j]
                    if childTree != undefined do return childTree
                )
                undefined
            )
            fn explorerControl explorerName = (
                local forms = (dotNetClass "System.Windows.Forms.Application").OpenForms
                for j = 0 to (forms.Count - 1) do (
                    local form = forms.Item[j]
                    if (form.GetType()).FullName == "MaxCustomControls.SceneExplorerControls.SceneExplorerDialog" do (
                        local control = form.ExplorerControl
                        if control.InstanceName == explorerName do return control
                    )
                )
                undefined
            )
            for i = 1 to SceneExplorerManager.GetExplorerCount() do (
                local n = SceneExplorerManager.GetExplorerName i
                if SceneExplorerManager.ExplorerIsOpen n do (
                    local explorer = SceneExplorerManager.GetExplorer n
                    if explorer != undefined do (
                        local control = explorerControl n
                        if control != undefined and not (isTarget explorer target) do control.PullSelectionFromScene()
                        if isTarget explorer target do (
                            -- ExpandSelected recursively opens descendants. Only open the target row.
                            if control != undefined do (
                                local tree = findTree control
                                if tree != undefined and tree.Selection.Count == 1 do (
                                    local row = tree.Selection.Item[0]
                                    row.Expanded = true
                                )
                            )
                            explorer.FindSelected()
                        )
                    )
                )
            )
        )
    )";
    ExecuteMAXScriptScript(script, MAXScript::ScriptSource::NonEmbedded, TRUE);
}
