#pragma once

#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

struct FWyVehicleTextureRow;

class SWyVehicleTexturePanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SWyVehicleTexturePanel) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual void Tick(const FGeometry& AllottedGeometry, double CurrentTime, float DeltaTime) override;

private:
    using FRowPtr = TSharedPtr<FWyVehicleTextureRow>;

    TSharedRef<SWidget> BuildTexturePage();
    TSharedRef<ITableRow> GenerateRow(FRowPtr Item, const TSharedRef<STableViewBase>& OwnerTable);
    TSharedPtr<SWidget> OpenContextMenu();
    FReply Refresh();
    FReply ApplyRow(FRowPtr Row);
    void ApplyRows(TArray<FRowPtr> SelectedRows);
    void UpdateFromBrowser(bool bForceRefresh);

    TArray<FRowPtr> Rows;
    TSharedPtr<SListView<FRowPtr>> List;
    FName BrowserFolder;
    FString VehicleRoot;
    FText ApplyError;
    double NextFolderCheckTime = 0.0;
};
