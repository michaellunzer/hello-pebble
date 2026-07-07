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
        "defaultValue": "1",
        "options": [
          {
            "label": "Oceania & SE Asia",
            "value": "0"
          },
          {
            "label": "Americas (California)",
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
