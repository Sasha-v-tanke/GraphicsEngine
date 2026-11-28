MODULE(Assets)

API(
    asset_id.h
    asset_meta.h
)

SOURCES(asset_meta.cpp)

PUBLIC_DEPENDS(Common)

RECURSE(test)
