module.exports = [
  {
    "type": "heading",
    "defaultValue": "Blue Pixel Settings"
  },
  {
    "type": "section",
    "items": [
      {
        "type": "toggle",
        "messageKey": "SHOW_DATE",
        "label": "Show Date",
        "defaultValue": true
      },
      {
        "type": "select",
        "messageKey": "GLOBE_VIEW",
        "label": "Globe View",
        "defaultValue": "0",
        "options": [
          {
            "label": "Australia",
            "value": "0"
          },
          {
            "label": "North America",
            "value": "1"
          }
        ]
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save Settings"
  }
];
