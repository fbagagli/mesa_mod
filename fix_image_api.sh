sed -i 's/dri2_dpy->image->createImage/dri_create_image/g' mesa/src/egl/drivers/dri2/platform_android.c
sed -i 's/dri2_dpy->image->mapImage/dri2_map_image/g' mesa/src/egl/drivers/dri2/platform_android.c
sed -i 's/dri2_dpy->image->unmapImage/dri2_unmap_image/g' mesa/src/egl/drivers/dri2/platform_android.c
