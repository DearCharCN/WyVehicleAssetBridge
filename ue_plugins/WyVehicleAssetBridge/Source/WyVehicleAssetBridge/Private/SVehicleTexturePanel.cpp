#include "SVehicleTexturePanel.h"

#include "AssetThumbnail.h"
#include "ContentBrowserItemPath.h"
#include "ContentBrowserModule.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IContentBrowserSingleton.h"
#include "Modules/ModuleManager.h"
#include "Styling/StyleColors.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "VehicleTextureAssignments.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"

#define LOCTEXT_NAMESPACE "WyVehicleTexturePanel"

namespace WyVehicleTexturePanel
{
    const FName TextureTabId(TEXT("WyVehicleAssetBridge.TextureAssignments"));
    const FName MaterialColumn(TEXT("Material"));
    const FName ParameterColumn(TEXT("Parameter"));
    const FName TextureColumn(TEXT("Texture"));
    const FName ApplyColumn(TEXT("Apply"));
    using FRowPtr = TSharedPtr<FWyVehicleTextureRow>;

    class SAssignmentRow : public SMultiColumnTableRow<FRowPtr>
    {
    public:
        SLATE_BEGIN_ARGS(SAssignmentRow) {}
            SLATE_ARGUMENT(FRowPtr, Item)
            SLATE_EVENT(FOnClicked, OnApply)
        SLATE_END_ARGS()

        void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
        {
            Item = InArgs._Item;
            OnApply = InArgs._OnApply;
            FSuperRowType::Construct(FTableRowArgs().Padding(FMargin(4, 2)), OwnerTable);
        }

        virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
        {
            if (Column == ApplyColumn)
            {
                return SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
                [
                    SNew(SWidgetSwitcher)
                    .WidgetIndex_Lambda([Row = Item] { return Row->bApplied ? 1 : 0; })
                    + SWidgetSwitcher::Slot()
                    [
                        SNew(SButton).Text(LOCTEXT("Apply", "应用")).OnClicked(OnApply)
                    ]
                    + SWidgetSwitcher::Slot()
                    [
                        SNew(STextBlock).Text(LOCTEXT("Applied", "已应用"))
                    ]
                ];
            }
            if (Column == TextureColumn)
            {
                constexpr int32 ThumbnailSize = 40;
                const TSharedRef<FAssetThumbnail> Thumbnail = MakeShared<FAssetThumbnail>(
                    Item->Texture, ThumbnailSize, ThumbnailSize, UThumbnailManager::Get().GetSharedThumbnailPool());
                FAssetThumbnailConfig Config;
                Config.ThumbnailLabel = EThumbnailLabel::NoLabel;
                Config.bAllowRealTimeOnHovered = false;
                return SNew(SHorizontalBox)
                    .ToolTipText(FText::FromString(Item->Texture.GetObjectPathString()))
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(SBox).WidthOverride(ThumbnailSize).HeightOverride(ThumbnailSize)
                        .Visibility(EVisibility::HitTestInvisible)
                        [ Thumbnail->MakeThumbnailWidget(Config) ]
                    ]
                    + SHorizontalBox::Slot().VAlign(VAlign_Center).Padding(7, 0, 2, 0)
                    [
                        SNew(STextBlock).Text(FText::FromName(Item->Texture.AssetName))
                        .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                    ];
            }
            const bool bMaterial = Column == MaterialColumn;
            return SNew(SBox).VAlign(VAlign_Center).Padding(FMargin(4, 0))
            [
                SNew(STextBlock).Text(FText::FromName(bMaterial ? Item->Material.AssetName : Item->Parameter))
                .ToolTipText(bMaterial ? FText::FromString(Item->Material.GetObjectPathString()) : FText::FromName(Item->Parameter))
                .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
            ];
        }

    private:
        FRowPtr Item;
        FOnClicked OnApply;
    };
}

void SWyVehicleTexturePanel::Construct(const FArguments& InArgs)
{
    using namespace WyVehicleTexturePanel;
    ChildSlot
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(8, 6, 8, 0)
        [
            SNew(SSegmentedControl<FName>).Value(TextureTabId)
            + SSegmentedControl<FName>::Slot(TextureTabId).Text(LOCTEXT("Tab", "贴图赋值到材质"))
        ]
        + SVerticalBox::Slot().FillHeight(1)
        [ BuildTexturePage() ]
    ];
    UpdateFromBrowser(true);
}

TSharedRef<SWidget> SWyVehicleTexturePanel::BuildTexturePage()
{
    using namespace WyVehicleTexturePanel;
    return SNew(SBox).Padding(8)
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton).Text(LOCTEXT("Refresh", "刷新")).OnClicked(this, &SWyVehicleTexturePanel::Refresh)
            ]
            + SHorizontalBox::Slot().VAlign(VAlign_Center).Padding(10, 0, 0, 0)
            [
                SNew(STextBlock).Text_Lambda([this] { return FText::FromString(VehicleRoot); })
                .ToolTipText_Lambda([this] { return FText::FromString(VehicleRoot); })
                .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
            ]
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(STextBlock).Text_Lambda([this] { return ApplyError; })
            .ColorAndOpacity(FSlateColor(EStyleColor::Error)).AutoWrapText(true)
            .Visibility_Lambda([this] { return ApplyError.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
        ]
        + SVerticalBox::Slot().FillHeight(1)
        [
            SNew(SOverlay)
            + SOverlay::Slot()
            [
                SAssignNew(List, SListView<FRowPtr>)
                .ListItemsSource(&Rows).SelectionMode(ESelectionMode::Multi)
                .OnGenerateRow(this, &SWyVehicleTexturePanel::GenerateRow)
                .OnContextMenuOpening(this, &SWyVehicleTexturePanel::OpenContextMenu)
                .HeaderRow(
                    SNew(SHeaderRow)
                    + SHeaderRow::Column(MaterialColumn).DefaultLabel(LOCTEXT("Material", "材质名")).FillWidth(.36f)
                    + SHeaderRow::Column(ParameterColumn).DefaultLabel(LOCTEXT("Parameter", "字段名")).FillWidth(.18f)
                    + SHeaderRow::Column(TextureColumn).DefaultLabel(LOCTEXT("Texture", "贴图")).FillWidth(.46f)
                    + SHeaderRow::Column(ApplyColumn).DefaultLabel(LOCTEXT("Apply", "应用")).ManualWidth(78))
            ]
            + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(12, 30, 12, 0)
            [
                SNew(STextBlock)
                .Visibility_Lambda([this] { return Rows.IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
                .Text_Lambda([this] {
                    return VehicleRoot.IsEmpty() ? LOCTEXT("PickVehicle", "请在内容浏览器中打开一个载具变体文件夹。")
                        : LOCTEXT("UpToDate", "此载具没有可赋值的贴图。");
                })
            ]
        ]
    ];
}

void SWyVehicleTexturePanel::Tick(const FGeometry& AllottedGeometry, double CurrentTime, float DeltaTime)
{
    SCompoundWidget::Tick(AllottedGeometry, CurrentTime, DeltaTime);
    // Switching the active content browser need not broadcast a folder change.
    if (CurrentTime >= NextFolderCheckTime)
    {
        NextFolderCheckTime = CurrentTime + .3;
        UpdateFromBrowser(false);
    }
}

void SWyVehicleTexturePanel::UpdateFromBrowser(bool bForceRefresh)
{
    auto& Browser = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser").Get();
    const FContentBrowserItemPath Path = Browser.GetCurrentPath();
    const FName Folder = Path.HasInternalPath() ? Path.GetInternalPathName() : NAME_None;
    if (!bForceRefresh && Folder == BrowserFolder)
    {
        return;
    }
    BrowserFolder = Folder;
    const FString Root = Path.HasInternalPath() ? WyVehicleTextures::VehicleRootForFolder(Path.GetInternalPathString()) : FString();
    if (!bForceRefresh && VehicleRoot == Root)
    {
        return;
    }
    VehicleRoot = Root;
    List->ClearSelection();
    Rows.Reset();
    ApplyError = FText::GetEmpty();
    if (!VehicleRoot.IsEmpty())
    {
        WyVehicleTextures::Scan(VehicleRoot, Rows);
    }
    List->RequestListRefresh();
}

TSharedRef<ITableRow> SWyVehicleTexturePanel::GenerateRow(FRowPtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
    return SNew(WyVehicleTexturePanel::SAssignmentRow, OwnerTable).Item(Item)
        .OnApply(this, &SWyVehicleTexturePanel::ApplyRow, Item);
}

FReply SWyVehicleTexturePanel::Refresh()
{
    UpdateFromBrowser(true);
    return FReply::Handled();
}

FReply SWyVehicleTexturePanel::ApplyRow(FRowPtr Row)
{
    ApplyRows({ Row });
    return FReply::Handled();
}

void SWyVehicleTexturePanel::ApplyRows(TArray<FRowPtr> SelectedRows)
{
    UpdateFromBrowser(false);
    // A folder change replaces the entire list, including every row from an older selection.
    if (SelectedRows.IsEmpty() || !Rows.Contains(SelectedRows[0]))
    {
        return;
    }
    SelectedRows.RemoveAll([](const FRowPtr& Row) { return Row->bApplied; });
    if (!SelectedRows.IsEmpty())
    {
        ApplyError = WyVehicleTextures::Apply(VehicleRoot, SelectedRows);
        List->RequestListRefresh();
    }
}

TSharedPtr<SWidget> SWyVehicleTexturePanel::OpenContextMenu()
{
    TArray<FRowPtr> SelectedRows = List->GetSelectedItems();
    if (SelectedRows.IsEmpty())
    {
        return nullptr;
    }
    const bool bHasPending = SelectedRows.ContainsByPredicate([](const FRowPtr& Row) { return !Row->bApplied; });
    FMenuBuilder Menu(true, nullptr);
    Menu.AddMenuEntry(LOCTEXT("Apply", "应用"), LOCTEXT("ApplySelected", "为选中的字段赋值。"), FSlateIcon(),
        FUIAction(FExecuteAction::CreateSP(this, &SWyVehicleTexturePanel::ApplyRows, MoveTemp(SelectedRows)),
            FCanExecuteAction::CreateLambda([bHasPending] { return bHasPending; })));
    return Menu.MakeWidget();
}

#undef LOCTEXT_NAMESPACE
