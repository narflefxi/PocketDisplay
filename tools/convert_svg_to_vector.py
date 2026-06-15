#!/usr/bin/env python3
"""
Convert SVG cursors to Android Vector Drawables with white fill + black outline.
"""

import xml.etree.ElementTree as ET
import re
import os

def parse_path_data(d):
    """Parse SVG path data and convert to Android format if needed."""
    # Android uses same path commands as SVG
    return d.strip()

def create_vector_drawable(name, viewport_width, viewport_height, paths, hotspot_offset_x=0, hotspot_offset_y=0, scale=1.0):
    """
    Create Android Vector Drawable XML.

    Args:
        paths: List of (path_data, fill_color, stroke_color, stroke_width) tuples
        hotspot_offset_x, hotspot_offset_y: Translation to align hotspot to (0,0)
        scale: Scale factor for the drawable
    """
    # Build XML
    ns = 'http://schemas.android.com/apk/res/android'
    root = ET.Element('vector')
    root.set(f'{{xmlns}}android', ns)
    root.set(f'{{xmlns}}android', 'http://schemas.android.com/apk/res/android')
    root.set('android:width', '24dp')
    root.set('android:height', '24dp')
    root.set('android:viewportWidth', str(viewport_width))
    root.set('android:viewportHeight', str(viewport_height))

    # Add group for offset/scale if needed
    needs_group = hotspot_offset_x != 0 or hotspot_offset_y != 0 or scale != 1.0

    parent = root
    if needs_group:
        group = ET.SubElement(root, 'group')
        transforms = []
        if scale != 1.0:
            transforms.append(f'scale({scale},{scale})')
        if hotspot_offset_x != 0 or hotspot_offset_y != 0:
            transforms.append(f'translate({hotspot_offset_x},{hotspot_offset_y})')
        group.set('android:translateX', str(hotspot_offset_x))
        group.set('android:translateY', str(hotspot_offset_y))
        parent = group

    for path_data, fill_color, stroke_color, stroke_width in paths:
        path_elem = ET.SubElement(parent, 'path')
        path_elem.set('android:pathData', path_data)
        if fill_color:
            path_elem.set('android:fillColor', fill_color)
        if stroke_color:
            path_elem.set('android:strokeColor', stroke_color)
            if stroke_width:
                path_elem.set('android:strokeWidth', str(stroke_width))

    # Pretty print
    ET.indent(root, space='    ')
    xml_str = ET.tostring(root, encoding='unicode', xml_declaration=True)
    xml_str = xml_str.replace(f'xmlns:ns0="{ns}" ', '')
    xml_str = xml_str.replace(f' ns0="{ns}"', '')
    xml_str = xml_str.replace('ns0:', 'android:')

    return xml_str

def main():
    output_dir = '../android/app/src/main/res/drawable'

    # Ensure output directory exists
    os.makedirs(output_dir, exist_ok=True)

    # arrow.svg - two paths (white body + dark outline), hotspot at tip (0,0)
    # ViewBox: 0 0 1024 1024
    # The arrow tip is at approximately (380, 210) based on the path data
    # We need to translate so tip is at (0,0)
    arrow_paths = [
        # White body
        ("M593.066667 846.933333c-2.133333 0-4.266667 0-8.533334-2.133333s-8.533333-6.4-12.8-10.666667l-78.933333-183.466666-96 89.6c-2.133333 4.266667-6.4 6.4-12.8 6.4-2.133333 0-6.4 0-8.533333-2.133334-6.4-2.133333-12.8-10.666667-12.8-19.2V256c0-8.533333 4.266667-17.066667 12.8-19.2 2.133333-2.133333 6.4-2.133333 8.533333-2.133333 4.266667 0 10.666667 2.133333 14.933333 6.4l341.333334 320c6.4 6.4 8.533333 14.933333 6.4 23.466666-2.133333 8.533333-10.666667 12.8-19.2 14.933334l-134.4 12.8 83.2 181.333333c2.133333 4.266667 2.133333 10.666667 0 17.066667-2.133333 4.266667-6.4 10.666667-10.666667 12.8l-61.866667 27.733333c-4.266667-4.266667-8.533333-4.266667-10.666666-4.266667z",
         "#FFFFFF", None, None),
        # Black outline
        ("M384 256l341.333333 320-164.266666 14.933333 96 209.066667-61.866667 27.733333-91.733333-211.2L384 725.333333V256m0-42.666667c-6.4 0-10.666667 2.133333-17.066667 4.266667-14.933333 6.4-25.6 21.333333-25.6 38.4v469.333333c0 17.066667 10.666667 32 25.6 38.4 6.4 4.266667 12.8 4.266667 17.066667 4.266667 10.666667 0 21.333333-4.266667 29.866667-10.666667l72.533333-68.266666 66.133333 155.733333c4.266667 10.666667 12.8 19.2 23.466667 23.466667 4.266667 2.133333 10.666667 2.133333 14.933333 2.133333 6.4 0 10.666667-2.133333 17.066667-4.266667l61.866667-27.733333c10.666667-4.266667 19.2-12.8 23.466666-23.466667 4.266667-10.666667 4.266667-23.466667 0-32l-70.4-153.6 104.533334-8.533333c17.066667-2.133333 32-12.8 36.266666-27.733333 6.4-14.933333 2.133333-34.133333-10.666666-44.8l-341.333334-320c-6.4-10.666667-17.066667-14.933333-27.733333-14.933334z",
         "#000000", None, None),
    ]
    # Arrow tip is roughly at (380, 213) in the 1024x1024 viewBox
    # Scale down and translate so tip is at origin
    arrow_xml = create_vector_drawable('arrow', 1024, 1024, arrow_paths, hotspot_offset_x=-380, hotspot_offset_y=-213, scale=0.025)
    with open(f'{output_dir}/cursor_arrow.xml', 'w') as f:
        f.write(arrow_xml)

    print("Created cursor_arrow.xml")

if __name__ == '__main__':
    main()
